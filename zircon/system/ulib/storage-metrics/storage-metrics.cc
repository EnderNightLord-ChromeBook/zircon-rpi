// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/block/cpp/banjo.h>

#include <algorithm>

#include <storage-metrics/block-metrics.h>
#include <storage-metrics/fs-metrics.h>
#include <storage-metrics/storage-metrics.h>

namespace storage_metrics {

namespace {
constexpr uint32_t block_operation(uint32_t command) { return command & BLOCK_OP_MASK; }
}  // namespace

bool RawCallStatEqual(const CallStatRawFidl& lhs, const CallStatRawFidl& rhs) {
  return (lhs.total_calls == rhs.total_calls) && (lhs.bytes_transferred == rhs.bytes_transferred);
}

bool CallStatEqual(const CallStatFidl& lhs, const CallStatFidl& rhs) {
  return RawCallStatEqual(lhs.success, rhs.success) && RawCallStatEqual(lhs.failure, rhs.failure);
}

bool BlockStatEqual(const BlockStatFidl& lhs, const BlockStatFidl& rhs) {
  return CallStatEqual(lhs.read, rhs.read) && CallStatEqual(lhs.write, rhs.write) &&
         CallStatEqual(lhs.trim, rhs.trim) && CallStatEqual(lhs.flush, rhs.flush) &&
         CallStatEqual(lhs.barrier_before, rhs.barrier_before) &&
         CallStatEqual(lhs.barrier_after, rhs.barrier_after);
}

CallStat::CallStatRaw::CallStatRaw() { Reset(); }

void CallStat::CallStatRaw::Reset() {
  minimum_latency = 0;
  maximum_latency = 0;
  total_time_spent = 0;
  total_calls = 0;
  bytes_transferred = 0;
  minimum_latency = kUninitializedMinimumLatency;
}

void CallStat::CallStatRaw::CopyFromRawFidl(const CallStatRawFidl* istat) {
  minimum_latency = istat->minimum_latency;
  maximum_latency = istat->maximum_latency;
  total_time_spent = istat->total_time_spent;
  total_calls = istat->total_calls;
  bytes_transferred = istat->bytes_transferred;
  minimum_latency = istat->minimum_latency;
}

void CallStat::CallStatRaw::CopyFromRawFidl(const fuchsia_storage_metrics_CallStatRaw* istat) {
  minimum_latency = istat->minimum_latency;
  maximum_latency = istat->maximum_latency;
  total_time_spent = istat->total_time_spent;
  total_calls = istat->total_calls;
  bytes_transferred = istat->bytes_transferred;
  minimum_latency = istat->minimum_latency;
}

void CallStat::CallStatRaw::CopyToRawFidl(CallStatRawFidl* out) const {
  out->minimum_latency = minimum_latency.load();
  out->maximum_latency = maximum_latency.load();
  out->total_time_spent = total_time_spent.load();
  out->total_calls = total_calls.load();
  out->bytes_transferred = bytes_transferred.load();
  out->minimum_latency = minimum_latency.load();
}

void CallStat::CallStatRaw::CopyToRawFidl(fuchsia_storage_metrics_CallStatRaw* out) const {
  out->minimum_latency = minimum_latency.load();
  out->maximum_latency = maximum_latency.load();
  out->total_time_spent = total_time_spent.load();
  out->total_calls = total_calls.load();
  out->bytes_transferred = bytes_transferred.load();
  out->minimum_latency = minimum_latency.load();
}

void CallStat::CallStatRaw::UpdateRawCallStat(zx_ticks_t delta_time, uint64_t bytes) {
  total_calls++;
  total_time_spent += delta_time;
  bytes_transferred += bytes;

  zx_ticks_t max = maximum_latency.load();
  while (delta_time > max) {
    maximum_latency.compare_exchange_strong(max, delta_time);
    max = maximum_latency.load();
  }

  zx_ticks_t min = minimum_latency.load();
  while (delta_time < min) {
    minimum_latency.compare_exchange_strong(min, delta_time);
    min = minimum_latency.load();
  }
}

CallStat::CallStat() { Reset(); }

void CallStat::Reset() {
  success_stat_.Reset();
  failure_stat_.Reset();
}

void CallStat::CopyFromFidl(const CallStatFidl* stat) {
  success_stat_.CopyFromRawFidl(&stat->success);
  failure_stat_.CopyFromRawFidl(&stat->failure);
}

void CallStat::CopyFromFidl(const fuchsia_storage_metrics_CallStat* stat) {
  success_stat_.CopyFromRawFidl(&stat->success);
  failure_stat_.CopyFromRawFidl(&stat->failure);
}

void CallStat::CopyToFidl(CallStatFidl* out) const {
  success_stat_.CopyToRawFidl(&out->success);
  failure_stat_.CopyToRawFidl(&out->failure);
}

void CallStat::CopyToFidl(fuchsia_storage_metrics_CallStat* out) const {
  success_stat_.CopyToRawFidl(&out->success);
  failure_stat_.CopyToRawFidl(&out->failure);
}

zx_ticks_t CallStat::minimum_latency(std::optional<bool> success) const {
  if (success) {
    if (*success == true) {
      return success_stat_.minimum_latency;
    } else {
      return failure_stat_.minimum_latency;
    }
  }
  return std::min(success_stat_.minimum_latency, failure_stat_.minimum_latency);
}
zx_ticks_t CallStat::maximum_latency(std::optional<bool> success) const {
  if (success) {
    if (*success == true) {
      return success_stat_.maximum_latency;
    } else {
      return failure_stat_.maximum_latency;
    }
  }
  return std::max(success_stat_.maximum_latency, failure_stat_.maximum_latency);
}
zx_ticks_t CallStat::total_time_spent(std::optional<bool> success) const {
  if (success) {
    if (*success == true) {
      return success_stat_.total_time_spent;
    } else {
      return failure_stat_.total_time_spent;
    }
  }
  return success_stat_.total_time_spent + failure_stat_.total_time_spent;
}
uint64_t CallStat::total_calls(std::optional<bool> success) const {
  if (success) {
    if (*success == true) {
      return success_stat_.total_calls;
    } else {
      return failure_stat_.total_calls;
    }
  }
  return success_stat_.total_calls + failure_stat_.total_calls;
}
uint64_t CallStat::bytes_transferred(std::optional<bool> success) const {
  if (success) {
    if (*success == true) {
      return success_stat_.bytes_transferred;
    } else {
      return failure_stat_.bytes_transferred;
    }
  }
  return success_stat_.bytes_transferred + failure_stat_.bytes_transferred;
}

void CallStat::Dump(FILE* stream, const char* stat_name, std::optional<bool> success) const {
  const char* stat_success = "aggregate";
  if (success) {
    if (*success) {
      stat_success = "success";
    } else {
      stat_success = "failure";
    }
  }
  fprintf(stream, "%s.%s.total_calls:         %lu\n", stat_name, stat_success,
          total_calls(success));
  fprintf(stream, "%s.%s.total_time_spent:    %ld\n", stat_name, stat_success,
          total_time_spent(success));
  fprintf(stream, "%s.%s.maximum_latency:     %ld\n", stat_name, stat_success,
          maximum_latency(success));
  fprintf(stream, "%s.%s.minimum_latency:     %ld\n", stat_name, stat_success,
          minimum_latency(success) == kUninitializedMinimumLatency ? 0 : minimum_latency(success));
  fprintf(stream, "%s.%s.bytes_transferred:   %lu\n", stat_name, stat_success,
          bytes_transferred(success));
  fprintf(stream, "\n");
}

void CallStat::UpdateCallStat(bool success, zx_ticks_t delta_time, uint64_t bytes_transferred) {
  CallStatRaw* stat;
  if (success) {
    stat = &success_stat_;
  } else {
    stat = &failure_stat_;
  }

  stat->UpdateRawCallStat(delta_time, bytes_transferred);
}

void Metrics::SetEnable(bool enable) { enabled_ = enable; }

bool Metrics::Enabled() const { return enabled_; }

FsMetrics::FsMetrics(const ::llcpp::fuchsia::storage::metrics::FsMetrics* metrics) {
  create_.CopyFromFidl(&metrics->create);
  read_.CopyFromFidl(&metrics->read);
  write_.CopyFromFidl(&metrics->write);
  truncate_.CopyFromFidl(&metrics->truncate);
  unlink_.CopyFromFidl(&metrics->unlink);
  rename_.CopyFromFidl(&metrics->rename);
  lookup_.CopyFromFidl(&metrics->lookup);
  open_.CopyFromFidl(&metrics->open);

  SetEnable(true);
}

FsMetrics::FsMetrics(const fuchsia_storage_metrics_FsMetrics* metrics) {
  create_.CopyFromFidl(&metrics->create);
  read_.CopyFromFidl(&metrics->read);
  write_.CopyFromFidl(&metrics->write);
  truncate_.CopyFromFidl(&metrics->truncate);
  unlink_.CopyFromFidl(&metrics->unlink);
  rename_.CopyFromFidl(&metrics->rename);
  lookup_.CopyFromFidl(&metrics->lookup);
  open_.CopyFromFidl(&metrics->open);

  SetEnable(true);
}

void FsMetrics::CopyToFidl(::llcpp::fuchsia::storage::metrics::FsMetrics* metrics) const {
  create_.CopyToFidl(&metrics->create);
  read_.CopyToFidl(&metrics->read);
  write_.CopyToFidl(&metrics->write);
  truncate_.CopyToFidl(&metrics->truncate);
  unlink_.CopyToFidl(&metrics->unlink);
  rename_.CopyToFidl(&metrics->rename);
  lookup_.CopyToFidl(&metrics->lookup);
  open_.CopyToFidl(&metrics->open);
}

void FsMetrics::CopyToFidl(fuchsia_storage_metrics_FsMetrics* metrics) const {
  create_.CopyToFidl(&metrics->create);
  read_.CopyToFidl(&metrics->read);
  write_.CopyToFidl(&metrics->write);
  truncate_.CopyToFidl(&metrics->truncate);
  unlink_.CopyToFidl(&metrics->unlink);
  rename_.CopyToFidl(&metrics->rename);
  lookup_.CopyToFidl(&metrics->lookup);
  open_.CopyToFidl(&metrics->open);
}

void FsMetrics::Dump(FILE* stream, std::optional<bool> success) const {
  create_.Dump(stream, "create", success);
  read_.Dump(stream, "read", success);
  write_.Dump(stream, "write", success);
  truncate_.Dump(stream, "truncate", success);
  unlink_.Dump(stream, "unlink", success);
  rename_.Dump(stream, "rename", success);
  lookup_.Dump(stream, "lookup", success);
  open_.Dump(stream, "open", success);
}

BlockDeviceMetrics::BlockDeviceMetrics(const BlockStatFidl* metrics) {
  read_.CopyFromFidl(&metrics->read);
  write_.CopyFromFidl(&metrics->write);
  trim_.CopyFromFidl(&metrics->trim);
  flush_.CopyFromFidl(&metrics->flush);
  barrier_before_.CopyFromFidl(&metrics->barrier_before);
  barrier_after_.CopyFromFidl(&metrics->barrier_after);

  SetEnable(true);
}

BlockDeviceMetrics::BlockDeviceMetrics(const fuchsia_hardware_block_BlockStats* metrics) {
  read_.CopyFromFidl(&metrics->read);
  write_.CopyFromFidl(&metrics->write);
  trim_.CopyFromFidl(&metrics->trim);
  flush_.CopyFromFidl(&metrics->flush);
  barrier_before_.CopyFromFidl(&metrics->barrier_before);
  barrier_after_.CopyFromFidl(&metrics->barrier_after);

  SetEnable(true);
}

void BlockDeviceMetrics::CopyToFidl(BlockStatFidl* metrics) const {
  read_.CopyToFidl(&metrics->read);
  write_.CopyToFidl(&metrics->write);
  trim_.CopyToFidl(&metrics->trim);
  flush_.CopyToFidl(&metrics->flush);
  barrier_before_.CopyToFidl(&metrics->barrier_before);
  barrier_after_.CopyToFidl(&metrics->barrier_after);
}

void BlockDeviceMetrics::CopyToFidl(fuchsia_hardware_block_BlockStats* metrics) const {
  read_.CopyToFidl(&metrics->read);
  write_.CopyToFidl(&metrics->write);
  trim_.CopyToFidl(&metrics->trim);
  flush_.CopyToFidl(&metrics->flush);
  barrier_before_.CopyToFidl(&metrics->barrier_before);
  barrier_after_.CopyToFidl(&metrics->barrier_after);
}

uint64_t BlockDeviceMetrics::TotalCalls(std::optional<bool> success) const {
  // Barriers are modifiers of a command so we don't count them here
  return read_.total_calls(success) + write_.total_calls(success) + trim_.total_calls(success) +
         flush_.total_calls(success);
}

uint64_t BlockDeviceMetrics::TotalBytesTransferred(std::optional<bool> success) const {
  // Barriers are modifiers of a command so we don't count their bytes
  return read_.bytes_transferred(success) + write_.bytes_transferred(success) +
         trim_.bytes_transferred(success) + flush_.bytes_transferred(success);
}

void BlockDeviceMetrics::Dump(FILE* stream, std::optional<bool> success) const {
  read_.Dump(stream, "read", success);
  write_.Dump(stream, "write", success);
  trim_.Dump(stream, "trim", success);
  flush_.Dump(stream, "flush", success);
  barrier_before_.Dump(stream, "barrier_before", success);
  barrier_after_.Dump(stream, "barrier_after", success);
}

void BlockDeviceMetrics::UpdateStats(bool success, const zx::ticks start_tick,
                                     const uint32_t command, const uint64_t bytes_transfered) {
  zx::ticks duration = zx::ticks::now() - start_tick;

  if (block_operation(command) == BLOCK_OP_WRITE) {
    UpdateWriteStat(success, duration.get(), bytes_transfered);
  } else if (block_operation(command) == BLOCK_OP_READ) {
    UpdateReadStat(success, duration.get(), bytes_transfered);
  } else if (block_operation(command) == BLOCK_OP_FLUSH) {
    UpdateFlushStat(success, duration.get(), bytes_transfered);
  } else if (block_operation(command) == BLOCK_OP_TRIM) {
    UpdateTrimStat(success, duration.get(), bytes_transfered);
  }

  if ((command & BLOCK_FL_BARRIER_BEFORE) == BLOCK_FL_BARRIER_BEFORE) {
    UpdateBarrierBeforeStat(success, duration.get(), bytes_transfered);
  }

  if ((command & BLOCK_FL_BARRIER_AFTER) == BLOCK_FL_BARRIER_AFTER) {
    UpdateBarrierAfterStat(success, duration.get(), bytes_transfered);
  }
}

}  // namespace storage_metrics
