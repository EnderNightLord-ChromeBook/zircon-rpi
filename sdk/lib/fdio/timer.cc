// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/sync/mutex.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <lib/zx/timer.h>
#include <lib/zxio/null.h>
#include <lib/zxio/ops.h>
#include <sys/timerfd.h>
#include <time.h>
#include <zircon/assert.h>
#include <zircon/syscalls.h>

#include <algorithm>
#include <utility>

#include <fbl/auto_call.h>

#include "fdio_unistd.h"
#include "internal.h"

// An implementation of a POSIX timerfd.
typedef struct fdio_timer {
  zxio_t io;

  // The zx::timer object that implements the timerfd.
  zx::timer handle;

  sync_mutex_t lock;

  zx::time current_deadline __TA_GUARDED(lock);
  zx::duration interval __TA_GUARDED(lock);
} fdio_timer_t;

static_assert(sizeof(fdio_timer_t) <= sizeof(zxio_storage_t),
              "fdio_timer_t must fit inside zxio_storage_t.");

static struct timespec duration_to_timespec(zx::duration duration) {
  struct timespec result = {};
  result.tv_sec = duration.to_secs();
  result.tv_nsec = duration % zx::sec(1);
  return result;
}

static bool timespec_to_duration(const struct timespec* spec, zx::duration* out_duration) {
  if (!spec || spec->tv_sec < 0 || spec->tv_nsec < 0 || spec->tv_sec > INT64_MAX / ZX_SEC(1)) {
    return false;
  }
  *out_duration = zx::sec(spec->tv_sec) + zx::nsec(spec->tv_nsec);
  return true;
}

static zx_status_t fdio_timer_close(zxio_t* io) {
  auto* timer = reinterpret_cast<fdio_timer_t*>(io);
  timer->~fdio_timer_t();
  return ZX_OK;
}

static zx_status_t fdio_timer_readv(zxio_t* io, const zx_iovec_t* vector, size_t vector_count,
                                    zxio_flags_t flags, size_t* out_actual) {
  if (fdio_iovec_get_capacity(vector, vector_count) < sizeof(uint64_t)) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }

  fdio_timer_t* timer = reinterpret_cast<fdio_timer_t*>(io);

  sync_mutex_lock(&timer->lock);
  if (timer->current_deadline == zx::time()) {
    // The timer was never set.
    sync_mutex_unlock(&timer->lock);
    return ZX_ERR_SHOULD_WAIT;
  }

  zx::time now = zx::clock::get_monotonic();
  if (timer->current_deadline > now) {
    sync_mutex_unlock(&timer->lock);
    return ZX_ERR_SHOULD_WAIT;
  }

  uint64_t count = 1;
  if (timer->interval > zx::duration()) {
    count = (now - timer->current_deadline) / timer->interval + 1;
    timer->current_deadline += timer->interval * count;
    // After reading the current value, the timer will no longer be readable until we reach the next
    // deadline. Calling zx_timer_set will clear the ZX_TIMER_SIGNALED signal until at least then.
    zx_status_t status = timer->handle.set(timer->current_deadline, zx::duration());
    ZX_ASSERT(status == ZX_OK);
  } else {
    timer->current_deadline = zx::time();
    // After reading the current value for non-repeating timer, the timer will never produce any
    // more values, so we use zx_timer_cancel to clear the ZX_TIMER_SIGNALED signal.
    zx_status_t status = timer->handle.cancel();
    ZX_ASSERT(status == ZX_OK);
  }

  fdio_iovec_copy_to(reinterpret_cast<const uint8_t*>(&count), sizeof(count), vector, vector_count,
                     out_actual);

  sync_mutex_unlock(&timer->lock);
  return ZX_OK;
}

static void fdio_timer_wait_begin(zxio_t* io, zxio_signals_t zxio_signals, zx_handle_t* out_handle,
                                  zx_signals_t* out_zx_signals) {
  fdio_timer_t* timer = reinterpret_cast<fdio_timer_t*>(io);
  zx_signals_t zx_signals = ZX_SIGNAL_NONE;
  if (zxio_signals & ZXIO_SIGNAL_READABLE) {
    zx_signals |= ZX_TIMER_SIGNALED;
  }
  *out_handle = timer->handle.get();
  *out_zx_signals = zx_signals;
}

static void fdio_timer_wait_end(zxio_t* io, zx_signals_t zx_signals,
                                zxio_signals_t* out_zxio_signals) {
  zxio_signals_t zxio_signals = ZXIO_SIGNAL_NONE;
  if (zx_signals & ZX_TIMER_SIGNALED) {
    zxio_signals |= ZXIO_SIGNAL_READABLE;
  }
  *out_zxio_signals = zxio_signals;
}

static constexpr zxio_ops_t fdio_timer_ops = []() {
  zxio_ops_t ops = zxio_default_ops;
  ops.close = fdio_timer_close;
  ops.readv = fdio_timer_readv;
  ops.wait_begin = fdio_timer_wait_begin;
  ops.wait_end = fdio_timer_wait_end;
  return ops;
}();

static void fdio_timer_init(zxio_storage_t* storage, zx::timer handle) {
  auto timer = new (storage) fdio_timer_t{
    .io = storage->io,
    .handle = std::move(handle),
    .lock = {},
    .current_deadline = {},
    .interval = {},
  };
  zxio_init(&timer->io, &fdio_timer_ops);
}

static fdio_t* fdio_timer_create(zx::timer handle) {
  zxio_storage_t* storage = nullptr;
  fdio_t* io = fdio_zxio_create(&storage);
  if (io == nullptr) {
    return nullptr;
  }
  fdio_timer_init(storage, std::move(handle));
  return io;
}

static bool to_timer(fdio_t* io, fdio_timer_t** out_timer) {
  if (!io) {
    return false;
  }
  const zxio_ops_t* ops = zxio_get_ops(fdio_get_zxio(io));
  if (ops != &fdio_timer_ops) {
    return false;
  }
  *out_timer = reinterpret_cast<fdio_timer_t*>(fdio_get_zxio(io));
  return true;
}

__EXPORT
int timerfd_create(int clockid, int flags) {
  zx_clock_t zx_clock_id = ZX_CLOCK_MONOTONIC;
  switch (clockid) {
    case CLOCK_REALTIME:
      return ERRNO(ENOSYS);
    case CLOCK_MONOTONIC:
      zx_clock_id = ZX_CLOCK_MONOTONIC;
      break;
    default:
      return ERRNO(EINVAL);
  }

  if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
    // TODO: Implement TFD_TIMER_ABSTIME.
    return ERRNO(EINVAL);
  }

  zx::timer timer;
  zx_status_t status = zx::timer::create(ZX_TIMER_SLACK_LATE, zx_clock_id, &timer);
  if (status != ZX_OK) {
    return ERROR(status);
  }

  fdio_t* io = nullptr;
  if ((io = fdio_timer_create(std::move(timer))) == nullptr) {
    return ERROR(ZX_ERR_NO_MEMORY);
  }

  if (flags & TFD_CLOEXEC) {
    *fdio_get_ioflag(io) |= IOFLAG_CLOEXEC;
  }

  if (flags & TFD_NONBLOCK) {
    *fdio_get_ioflag(io) |= IOFLAG_NONBLOCK;
  }

  int fd = fdio_bind_to_fd(io, -1, 0);
  if (fd < 0) {
    fdio_release(io);
  }
  // fdio_bind_to_fd already sets errno.
  return fd;
}

static void fdio_timer_get_current_timespec(fdio_timer_t* timer, struct itimerspec* out_timespec)
    __TA_REQUIRES(timer->lock) {
  zx::time now = zx::clock::get_monotonic();
  if (timer->interval == zx::duration() && timer->current_deadline <= now) {
    out_timespec->it_value = duration_to_timespec(zx::duration());
  } else {
    // TODO: Is it ok for it_value if the caller is behind in reading a repeating timer?
    out_timespec->it_value = duration_to_timespec(timer->current_deadline - now);
  }
  out_timespec->it_interval = duration_to_timespec(timer->interval);
}

__EXPORT int timerfd_settime(int fd, int flags, const struct itimerspec* new_value,
                             struct itimerspec* old_value) {
  fdio_t* io = fd_to_io(fd);
  if (io == nullptr) {
    return ERRNO(EBADF);
  }
  auto clean_io = fbl::MakeAutoCall([io] { fdio_release(io); });

  fdio_timer_t* timer = nullptr;
  if (!to_timer(io, &timer)) {
    return ERRNO(EINVAL);
  }

  if (flags) {
    // TODO: Implement TFD_TIMER_ABSTIME.
    return ERRNO(EINVAL);
  }

  zx::duration value;
  if (!timespec_to_duration(&new_value->it_value, &value)) {
    return ERRNO(EINVAL);
  }
  zx::duration interval;
  if (!timespec_to_duration(&new_value->it_interval, &interval)) {
    return ERRNO(EINVAL);
  }

  sync_mutex_lock(&timer->lock);

  struct itimerspec old = {};
  if (old_value) {
    fdio_timer_get_current_timespec(timer, &old);
  }

  zx::time current_deadline = value.get() == 0 ? zx::time() : zx::deadline_after(value);
  zx_status_t status = ZX_OK;

  if (current_deadline > zx::time()) {
    status = timer->handle.set(current_deadline, zx::duration());
  } else {
    status = timer->handle.cancel();
  }

  if (status != ZX_OK) {
    sync_mutex_unlock(&timer->lock);
    return STATUS(status);
  }

  timer->current_deadline = current_deadline;
  timer->interval = interval;
  sync_mutex_unlock(&timer->lock);

  if (old_value) {
    *old_value = old;
  }
  return 0;
}

__EXPORT
int timerfd_gettime(int fd, struct itimerspec* curr_value) {
  fdio_t* io = fd_to_io(fd);
  if (io == nullptr) {
    return ERRNO(EBADF);
  }
  auto clean_io = fbl::MakeAutoCall([io] { fdio_release(io); });

  fdio_timer_t* timer = nullptr;
  if (!to_timer(io, &timer)) {
    return ERRNO(EINVAL);
  }
  sync_mutex_lock(&timer->lock);
  fdio_timer_get_current_timespec(timer, curr_value);
  sync_mutex_unlock(&timer->lock);
  return 0;
}
