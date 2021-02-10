// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains Vnodes which back a Blobfs filesystem.

#ifndef SRC_STORAGE_BLOBFS_BLOB_H_
#define SRC_STORAGE_BLOBFS_BLOB_H_

#ifndef __Fuchsia__
#error Fuchsia-only Header
#endif

#include <fuchsia/io/llcpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/fit/promise.h>
#include <lib/zx/event.h>
#include <string.h>

#include <memory>
#include <mutex>

#include <digest/digest.h>
#include <fbl/algorithm.h>
#include <fbl/intrusive_wavl_tree.h>
#include <fbl/macros.h>
#include <fbl/ref_ptr.h>
#include <fbl/vector.h>
#include <fs/journal/data_streamer.h>
#include <fs/vfs.h>
#include <fs/vfs_types.h>
#include <fs/vnode.h>
#include <storage/buffer/owned_vmoid.h>

#include "src/storage/blobfs/allocator/allocator.h"
#include "src/storage/blobfs/allocator/extent-reserver.h"
#include "src/storage/blobfs/allocator/node-reserver.h"
#include "src/storage/blobfs/blob-cache.h"
#include "src/storage/blobfs/blob-layout.h"
#include "src/storage/blobfs/common.h"
#include "src/storage/blobfs/compression/blob-compressor.h"
#include "src/storage/blobfs/compression/compressor.h"
#include "src/storage/blobfs/format-assertions.h"
#include "src/storage/blobfs/format.h"
#include "src/storage/blobfs/metrics.h"
#include "src/storage/blobfs/pager/page-watcher.h"
#include "src/storage/blobfs/transaction.h"

namespace blobfs {

class Blobfs;
class Producer;

using digest::Digest;

enum class BlobState : uint8_t {
  // After Open:
  kEmpty,
  // After Space Reserved (but allocation not yet persisted).
  kDataWrite,
  // After Writing:
  kReadable,
  // After Unlink:
  kPurged,
  // Unrecoverable error states:
  kError,
};

// clang-format on

class Blob final : public CacheNode, fbl::Recyclable<Blob> {
 public:
  // Constructs a blob, reads in data, verifies the contents, then destroys the in-memory copy.
  static zx_status_t LoadAndVerifyBlob(Blobfs* bs, uint32_t node_index);

  Blob(Blobfs* bs, const Digest& digest);

  // Creates a readable blob from existing data.
  Blob(Blobfs* bs, uint32_t node_index, const Inode& inode);

  virtual ~Blob();

  ////////////////
  // fs::Vnode interface.

  using fs::Vnode::Open;
  zx_status_t Open(ValidatedOptions options, fbl::RefPtr<Vnode>* out_redirect) final;
  zx_status_t Close() final;
  zx_status_t GetNodeInfoForProtocol(fs::VnodeProtocol protocol, fs::Rights rights,
                                     fs::VnodeRepresentation* info) final;
  fs::VnodeProtocolSet GetProtocols() const final;
  bool ValidateRights(fs::Rights rights) final;
  zx_status_t Read(void* data, size_t len, size_t off, size_t* out_actual) final;
  zx_status_t Write(const void* data, size_t len, size_t offset, size_t* out_actual) final;
  zx_status_t Append(const void* data, size_t len, size_t* out_end, size_t* out_actual) final;
  zx_status_t GetAttributes(fs::VnodeAttributes* a) final;
  zx_status_t Truncate(size_t len) final;
  zx_status_t QueryFilesystem(llcpp::fuchsia::io::FilesystemInfo* out) final;
  zx_status_t GetDevicePath(size_t buffer_len, char* out_name, size_t* out_len) final;
  zx_status_t GetVmo(int flags, zx::vmo* out_vmo, size_t* out_size) final;
  void Sync(SyncCallback on_complete) final;

  ////////////////
  // fbl::Recyclable interface.

  void fbl_recycle() final { CacheNode::fbl_recycle(); }

  ////////////////
  // Other methods.

  // Identifies if we can safely remove all on-disk and in-memory storage used by this blob.
  // Note that this *must* be called on the main dispatch thread; otherwise the underlying state of
  // the blob could change after (or during) the call, and the blob might not really be purgeable.
  bool Purgeable() const {
    return fd_count_ == 0 && !clone_ref_ && (DeletionQueued() || state() != BlobState::kReadable);
  }

  bool DeletionQueued() const { return deletable_; }

  uint32_t GetMapIndex() const { return map_index_; }

  // Returns a unique identifier for this blob
  uint32_t Ino() const { return map_index_; }

  uint64_t SizeData() const;

  const Inode& GetNode() const { return inode_; }

  void CompleteSync();

  // When blob VMOs are cloned and returned to clients, blobfs watches
  // the original VMO handle for the signal |ZX_VMO_ZERO_CHILDREN|.
  // While this signal is not set, the blob's Vnode keeps an extra
  // reference to itself to prevent teardown while clients are using
  // this Vmo. This reference is internally called the "clone watcher".
  //
  // This function may be called on a blob to tell it to forcefully release
  // the "reference to itself" that is kept when the blob is mapped.
  //
  // Returns this reference, if it exists, to provide control over
  // when the Vnode destructor is executed.
  fbl::RefPtr<Blob> CloneWatcherTeardown();

  // Marks the blob as deletable, and attempt to purge it.
  zx_status_t QueueUnlink();

  // PrepareWrite should be called after allocating a vnode and before writing any data to the blob.
  // The function sets blob size, allocates vmo needed for data and merkle tree, initiates
  // structures needed for compression and reserves an inode for the blob.  It is not meant to be
  // called multiple times on a given vnode.  This is public only for testing.
  zx_status_t PrepareWrite(uint64_t size_data, bool compress);

  // If this is part of a migration and involves writing a new blob to replace an old blob, this can
  // be called so that the blob is deleted in the transaction that writes the new blob.  The blob
  // *must* not be currently in use.  It is designed to be used for mount time migrations.
  void SetOldBlob(Blob& blob);

 private:
  DISALLOW_COPY_ASSIGN_AND_MOVE(Blob);

  ////////////////
  // blobfs::CacheNode interface.

  BlobCache& Cache() final;
  bool ShouldCache() const final;
  void ActivateLowMemory() final;

  ////////////////
  // Other methods.

  void set_state(BlobState new_state) { state_ = new_state; };
  BlobState state() const { return state_; }

  // After writing the blob, marks the blob as readable.
  [[nodiscard]] zx_status_t MarkReadable();

  // Returns a handle to an event which will be signalled when
  // the blob is readable.
  //
  // Returns "ZX_OK" if successful, otherwise the error code
  // will indicate the failure status.
  zx_status_t GetReadableEvent(zx::event* out);

  // Returns a clone of the blobfs VMO.
  //
  // Monitors the current VMO, keeping a reference to the Vnode
  // alive while the |out| VMO (and any clones it may have) are open.
  zx_status_t CloneDataVmo(zx_rights_t rights, zx::vmo* out_vmo, size_t* out_size);

  // Receives notifications when all clones vended by CloneDataVmo() are released.
  void HandleNoClones(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
                      const zx_packet_signal_t* signal);

  // Invokes |Purge()| if the vnode is purgeable.
  zx_status_t TryPurge();

  // Removes all traces of the vnode from blobfs.
  // The blob is not expected to be accessed again after this is called.
  zx_status_t Purge();

  // Schedules journal transaction prepared by PrepareWrite for the null blob.
  // Null blob doesn't have any data to write. They don't go through regular
  // Write()/WriteInternal path so we explicitly issue journaled write that
  // commits inode allocation and creation.
  zx_status_t WriteNullBlob();

  // If successful, allocates Blob Node and Blocks (in-memory)
  // kBlobStateEmpty --> kBlobStateDataWrite
  zx_status_t SpaceAllocate(uint32_t block_count);

  // Writes to either the Merkle Tree or the Data section,
  // depending on the state.
  zx_status_t WriteInternal(const void* data, size_t len, size_t* actual);

  // Reads from a blob.
  // Requires: kBlobStateReadable
  zx_status_t ReadInternal(void* data, size_t len, size_t off, size_t* actual);

  // Loads the blob's data and merkle from disk, and initializes the data/merkle VMOs.
  // If paging is enabled, the data VMO will be pager-backed and lazily loaded and verified as the
  // client accesses the pages.
  // If paging is disabled, the entire data VMO is loaded in and verified.
  //
  // Idempotent.
  zx_status_t LoadVmosFromDisk();

  // Initializes the data VMO for writing.  Idempotent.
  zx_status_t PrepareDataVmoForWriting();

  // Commits all the data pages of the blob into memory, i.e. reads them from disk.
  zx_status_t CommitDataBuffer();

  // Verifies the integrity of the in-memory Blob - operates on the entire blob at once.
  // LoadVmosFromDisk() must have already been called for this blob.
  zx_status_t Verify() const;

  // Called by the Vnode once the last write has completed, updating the
  // on-disk metadata.
  zx_status_t WriteMetadata(BlobTransaction& transaction);

  // Returns whether the data or merkle tree bytes are mapped and resident in memory.
  bool IsDataLoaded() const;
  bool IsMerkleTreeLoaded() const;

  // Acquires a pointer to the mapped data or merkle tree.
  // May return nullptr if the mappings have not been initialized.
  void* GetDataBuffer() const;
  void* GetMerkleTreeBuffer(const BlobLayout& blob_layout) const;

  // Returns whether the blob's contents are pager-backed or not.
  bool IsPagerBacked() const;

  // Returns a digest::Digest containing the blob's merkle root.
  // Equivalent to digest::Digest(GetKey()).
  digest::Digest MerkleRoot() const;

  // Commits the blob to persistent storage.
  zx_status_t Commit();

  // Returns the block size used by blobfs.
  uint32_t GetBlockSize() const;

  // Write |block_count| blocks using the data from |producer| into |streamer|.
  zx_status_t WriteData(uint32_t block_count, Producer& producer, fs::DataStreamer& streamer);

  Blobfs* const blobfs_;
  BlobState state_ = BlobState::kEmpty;
  // True if this node should be unlinked when closed.
  bool deletable_ = false;

  bool tearing_down_ = false;

  enum class SyncingState : char {
    // The Blob is being streamed and it is not possible to generate the merkle root and metadata at
    // this point.
    kDataIncomplete,

    // The blob merkle root is complete but the metadate write has not yet submitted to the
    // underlying media.
    kSyncing,

    // The blob exists on the underlying media.
    kDone,
  };
  // This value is marked kDone on the journal's background thread but read on the main thread so
  // is protected by the mutex.
  SyncingState syncing_state_ __TA_GUARDED(mutex_) = SyncingState::kDataIncomplete;

  uint32_t map_index_ = 0;

  // This object is not generally threadsafe but a few small things are done on the journal thread.
  // This mutex protects such data.
  std::mutex mutex_;

  // VMO mappings for the blob's merkle tree and data.
  // Data is stored in a separate VMO from the merkle tree for several reasons:
  //   - While data may be paged, the merkle tree (i.e. verification metadata) should always be
  //     retained.
  //   - VMO cloning when handing out a copy to read clients is simpler and requires no arithmetic.
  //   - Makes memory accounting more granular.
  // For small blobs, merkle_mapping_ may be absent, since small blobs may not have any stored
  // merkle tree.
  fzl::OwnedVmoMapper merkle_mapping_;
  fzl::OwnedVmoMapper data_mapping_;

  // Watches any clones of "vmo_" provided to clients.
  // Observes the ZX_VMO_ZERO_CHILDREN signal.
  async::WaitMethod<Blob, &Blob::HandleNoClones> clone_watcher_;
  // Keeps a reference to the blob alive (from within itself)
  // until there are no cloned VMOs in used.
  //
  // This RefPtr is only non-null when a client is using a cloned VMO,
  // or there would be a clear leak of Blob.
  fbl::RefPtr<Blob> clone_ref_ = {};

  zx::event readable_event_ = {};

  uint32_t fd_count_ = 0;

  // TODO(smklein): We are only using a few of these fields, such as:
  // - blob_size
  // - block_count
  // To save space, we could avoid holding onto the entire inode.
  Inode inode_ = {};

  // Data used exclusively during writeback.
  struct WriteInfo {
    // See comment for merkle_tree() below.
    static constexpr size_t kPreMerkleTreePadding = kBlobfsBlockSize;

    WriteInfo() = default;

    // Not copyable or movable because merkle_tree_creator has a pointer to digest.
    WriteInfo(const WriteInfo&) = delete;
    WriteInfo& operator=(const WriteInfo&) = delete;

    // We leave room in the merkle tree buffer to add padding before the merkle tree which might be
    // required with the compact blob layout.
    uint8_t* merkle_tree() const {
      ZX_ASSERT(merkle_tree_buffer);
      return merkle_tree_buffer.get() + kPreMerkleTreePadding;
    }

    uint64_t bytes_written = {};

    fbl::Vector<ReservedExtent> extents;
    fbl::Vector<ReservedNode> node_indices;

    std::optional<BlobCompressor> compressor;

    // The fused write error.  Once writing has failed, we return the same error on subsequent
    // writes in case a higher layer dropped the error and returned a short write instead.
    zx_status_t write_error = ZX_OK;

    // As data is written, we build the merkle tree using this.
    digest::MerkleTreeCreator merkle_tree_creator;

    // The merkle tree creator stores the root digest here.
    uint8_t digest[digest::kSha256Length];

    // The merkle tree creator stores the rest of the tree here.  The buffer includes space for
    // padding.  See the comment for merkle_tree() above.
    std::unique_ptr<uint8_t[]> merkle_tree_buffer;

    // The old blob that this write is replacing.
    fbl::RefPtr<Blob> old_blob;
  };

  std::unique_ptr<WriteInfo> write_info_ = {};

  // Reads in the blob's pages on demand.
  std::unique_ptr<pager::PageWatcher> page_watcher_ = nullptr;
};

// Returns true if the given inode supports paging.
bool SupportsPaging(const Inode& inode);

}  // namespace blobfs

#endif  // SRC_STORAGE_BLOBFS_BLOB_H_
