// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_APPMGR_LIFECYCLE_H_
#define SRC_SYS_APPMGR_LIFECYCLE_H_

#include <fuchsia/process/lifecycle/llcpp/fidl.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/llcpp/server.h>
#include <lib/zx/channel.h>

#include <memory>
#include <vector>

#include "fuchsia/process/lifecycle/cpp/fidl.h"

namespace component {

class Appmgr;

class LifecycleServer final : public llcpp::fuchsia::process::lifecycle::Lifecycle::Interface {
 public:
  LifecycleServer(Appmgr* appmgr, fit::function<void(zx_status_t)> stop_callback)
      : appmgr_(appmgr) {
    stop_callback_ = std::move(stop_callback);
  }

  zx_status_t Create(async_dispatcher_t* dispatcher, zx::channel chan);
  void Close(zx_status_t status);

  static zx_status_t Create(async_dispatcher_t* dispatcher, Appmgr* appmgr, zx::channel channel);

  void Stop(StopCompleter::Sync& completer) override;

 private:
  Appmgr* appmgr_;
  fit::function<void(zx_status_t)> stop_callback_;
  std::optional<fidl::ServerBindingRef<llcpp::fuchsia::process::lifecycle::Lifecycle>> lifecycle_;

  // For safe-keeping until appmgr shutsdown.
  std::vector<std::shared_ptr<fuchsia::process::lifecycle::LifecyclePtr>> child_lifecycles_;
};

}  // namespace component

#endif  // SRC_SYS_APPMGR_LIFECYCLE_H_
