// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef _ALL_SOURCE
#define _ALL_SOURCE  // Enables thrd_create_with_name in <threads.h>.
#endif

#include "src/storage/blobfs/compression/decompressor_sandbox/decompressor_impl.h"

#include <fuchsia/blobfs/internal/llcpp/fidl.h>
#include <fuchsia/scheduler/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fzl/owned-vmo-mapper.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <lib/zx/thread.h>
#include <lib/zx/time.h>
#include <threads.h>
#include <zircon/errors.h>
#include <zircon/status.h>
#include <zircon/threads.h>
#include <zircon/types.h>

#include <fbl/auto_call.h>
#include <src/lib/chunked-compression/chunked-decompressor.h>

#include "src/storage/blobfs/compression/decompressor.h"
#include "src/storage/blobfs/compression/external_decompressor.h"
#include "src/storage/blobfs/compression_settings.h"

namespace {
struct FifoInfo {
  zx::fifo fifo;
  fzl::OwnedVmoMapper compressed_mapper;
  fzl::OwnedVmoMapper decompressed_mapper;
};
}  // namespace

namespace blobfs {

// This will only decompress a set of complete chunks, if the beginning or end
// of the range are not chunk aligned this operation will fail.
zx_status_t DecompressChunkedPartial(const fzl::OwnedVmoMapper& decompressed_mapper,
                                     const fzl::OwnedVmoMapper& compressed_mapper,
                                     const llcpp::fuchsia::blobfs::internal::Range decompressed,
                                     const llcpp::fuchsia::blobfs::internal::Range compressed,
                                     size_t* bytes_decompressed) {
  const uint8_t* src = static_cast<const uint8_t*>(compressed_mapper.start()) + compressed.offset;
  uint8_t* dst = static_cast<uint8_t*>(decompressed_mapper.start()) + decompressed.offset;
  chunked_compression::ChunkedDecompressor decompressor;
  return decompressor.DecompressFrame(src, compressed.size, dst, decompressed.size,
                                      bytes_decompressed);
}

zx_status_t DecompressFull(const fzl::OwnedVmoMapper& decompressed_mapper,
                           const fzl::OwnedVmoMapper& compressed_mapper, size_t decompressed_length,
                           size_t compressed_length, CompressionAlgorithm algorithm,
                           size_t* bytes_decompressed) {
  std::unique_ptr<Decompressor> decompressor = nullptr;
  if (zx_status_t status = Decompressor::Create(algorithm, &decompressor); status != ZX_OK) {
    return status;
  }
  *bytes_decompressed = decompressed_length;
  return decompressor->Decompress(decompressed_mapper.start(), bytes_decompressed,
                                  compressed_mapper.start(), compressed_length);
}

// The actual handling of a request on the fifo.
void HandleFifo(const fzl::OwnedVmoMapper& compressed_mapper,
                const fzl::OwnedVmoMapper& decompressed_mapper,
                const llcpp::fuchsia::blobfs::internal::DecompressRequest* request,
                llcpp::fuchsia::blobfs::internal::DecompressResponse* response) {
  TRACE_DURATION("decompressor", "HandleFifo", "length", request->decompressed.size);
  size_t bytes_decompressed = 0;
  switch (request->algorithm) {
    case llcpp::fuchsia::blobfs::internal::CompressionAlgorithm::CHUNKED_PARTIAL:
      response->status =
          DecompressChunkedPartial(decompressed_mapper, compressed_mapper, request->decompressed,
                                   request->compressed, &bytes_decompressed);
      break;
    case llcpp::fuchsia::blobfs::internal::CompressionAlgorithm::LZ4:
    case llcpp::fuchsia::blobfs::internal::CompressionAlgorithm::ZSTD:
    case llcpp::fuchsia::blobfs::internal::CompressionAlgorithm::ZSTD_SEEKABLE:
    case llcpp::fuchsia::blobfs::internal::CompressionAlgorithm::CHUNKED:
      if (request->decompressed.offset != 0 || request->compressed.offset != 0) {
        bytes_decompressed = 0;
        response->status = ZX_ERR_NOT_SUPPORTED;
      } else {
        CompressionAlgorithm algorithm =
            ExternalDecompressorClient::CompressionAlgorithmFidlToLocal(request->algorithm);
        response->status =
            DecompressFull(decompressed_mapper, compressed_mapper, request->decompressed.size,
                           request->compressed.size, algorithm, &bytes_decompressed);
      }
      break;
    case llcpp::fuchsia::blobfs::internal::CompressionAlgorithm::UNCOMPRESSED:
      response->status = ZX_ERR_NOT_SUPPORTED;
      break;
  }

  response->size = bytes_decompressed;
}

// Watches a fifo for requests to take data from the compressed_vmo and
// extract the result into the memory region of decompressed_mapper.
void WatchFifo(zx::fifo fifo, fzl::OwnedVmoMapper compressed_mapper,
               fzl::OwnedVmoMapper decompressed_mapper) {
  constexpr zx_signals_t kFifoReadSignals = ZX_FIFO_READABLE | ZX_FIFO_PEER_CLOSED;
  constexpr zx_signals_t kFifoWriteSignals = ZX_FIFO_WRITABLE | ZX_FIFO_PEER_CLOSED;
  while (fifo.is_valid()) {
    zx_signals_t signal;
    fifo.wait_one(kFifoReadSignals, zx::time::infinite(), &signal);
    // It doesn't matter if there's anything left in the queue, nobody is there
    // to read the response.
    if ((signal & ZX_FIFO_PEER_CLOSED) != 0) {
      break;
    }
    llcpp::fuchsia::blobfs::internal::DecompressRequest request;
    zx_status_t status = fifo.read(sizeof(request), &request, 1, nullptr);
    if (status != ZX_OK) {
      break;
    }

    llcpp::fuchsia::blobfs::internal::DecompressResponse response;
    HandleFifo(compressed_mapper, decompressed_mapper, &request, &response);

    fifo.wait_one(kFifoWriteSignals, zx::time::infinite(), &signal);
    if ((signal & ZX_FIFO_WRITABLE) == 0 ||
        fifo.write(sizeof(response), &response, 1, nullptr) != ZX_OK) {
      break;
    }
  }
}

// A Wrapper around WatchFifo just to unwrap the data in the callback provided
// by thrd_create_With_name().
int WatchFifoWrapper(void* data) {
  std::unique_ptr<FifoInfo> info(static_cast<FifoInfo*>(data));
  WatchFifo(std::move(info->fifo), std::move(info->compressed_mapper),
            std::move(info->decompressed_mapper));
  return 0;
}

void SetDeadlineProfile(thrd_t* thread) {
  zx::channel channel0, channel1;
  zx_status_t status = zx::channel::create(0u, &channel0, &channel1);
  if (status != ZX_OK) {
    FX_LOGS(WARNING) << "[decompressor]: Could not create channel pair: "
                     << zx_status_get_string(status);
    return;
  }

  // Connect to the scheduler profile provider service.
  status = fdio_service_connect(
      (std::string("/svc/") + fuchsia::scheduler::ProfileProvider::Name_).c_str(),
      channel0.release());
  if (status != ZX_OK) {
    FX_LOGS(WARNING) << "[decompressor]: Could not connect to scheduler profile provider: "
                     << zx_status_get_string(status);
    return;
  }
  fuchsia::scheduler::ProfileProvider_SyncProxy profile_provider(std::move(channel1));

  // TODO(fxbug.dev/40858): Migrate to the role-based API when available, instead of hard
  // coding parameters.
  const zx_duration_t capacity = ZX_USEC(1000);
  const zx_duration_t deadline = ZX_MSEC(2);
  const zx_duration_t period = deadline;

  zx::profile profile;
  zx_status_t fidl_status = profile_provider.GetDeadlineProfile(
      capacity, deadline, period, "decompressor-fifo-thread", &status, &profile);

  if (status != ZX_OK || fidl_status != ZX_OK) {
    FX_LOGS(WARNING) << "[decompressor]: Failed to get deadline profile: "
                     << zx_status_get_string(status) << ", " << zx_status_get_string(fidl_status);
  } else {
    auto zx_thread = zx::unowned_thread(thrd_get_zx_handle(*thread));
    // Set the deadline profile.
    status = zx_thread->set_profile(profile, 0);
    if (status != ZX_OK) {
      FX_LOGS(WARNING) << "[decompressor]: Failed to set deadline profile: "
                       << zx_status_get_string(status);
    }
  }
}

void DecompressorImpl::Create(zx::fifo server_end, zx::vmo compressed_vmo, zx::vmo decompressed_vmo,
                              CreateCallback callback) {
  size_t vmo_size;
  zx_status_t status = decompressed_vmo.get_size(&vmo_size);
  if (status != ZX_OK) {
    return callback(status);
  }

  fzl::OwnedVmoMapper decompressed_mapper;
  status = decompressed_mapper.Map(std::move(decompressed_vmo), vmo_size);
  if (status != ZX_OK) {
    return callback(status);
  }

  status = compressed_vmo.get_size(&vmo_size);
  if (status != ZX_OK) {
    return callback(status);
  }

  fzl::OwnedVmoMapper compressed_mapper;
  status = compressed_mapper.Map(std::move(compressed_vmo), vmo_size, ZX_VM_PERM_READ);
  if (status != ZX_OK) {
    return callback(status);
  }

  thrd_t handler_thread;
  std::unique_ptr<FifoInfo> info = std::make_unique<FifoInfo>();
  *info = {std::move(server_end), std::move(compressed_mapper), std::move(decompressed_mapper)};
  if (thrd_create_with_name(&handler_thread, WatchFifoWrapper, info.release(),
                            "decompressor-fifo-thread") != thrd_success) {
    return callback(ZX_ERR_INTERNAL);
  }
  SetDeadlineProfile(&handler_thread);

  thrd_detach(handler_thread);
  return callback(ZX_OK);
}

}  // namespace blobfs
