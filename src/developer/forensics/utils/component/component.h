// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_UTILS_COMPONENT_COMPONENT_H_
#define SRC_DEVELOPER_FORENSICS_UTILS_COMPONENT_COMPONENT_H_

#include <fuchsia/process/lifecycle/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/sys/inspect/cpp/component.h>

#include <memory>

namespace forensics {
namespace component {

// Forensics components all use the same basic machinery to function. Component groups that
// machinery together and provides some additional information about the component instance that has
// been started.
//
// To properly use this class a component must have access to the "isolated-temp" feature in its
// sandbox and all instances of the component must have non-overlapping lifetimes and share the same
// namespace.
class Component {
 public:
  Component();

  async_dispatcher_t* Dispatcher();
  std::shared_ptr<sys::ServiceDirectory> Services();
  inspect::Node* InspectRoot();

  zx_status_t RunLoop();
  void ShutdownLoop();

  template <typename Interface>
  zx_status_t AddPublicService(::fidl::InterfaceRequestHandler<Interface> handler,
                               std::string service_name = Interface::Name_) {
    return context_->outgoing()->AddPublicService(std::move(handler), std::move(service_name));
  }

  // Returns true if this is the first time an instance of the current component has been started
  // since boot.
  bool IsFirstInstance() const;

  // Handle stopping the component when the stop signal is received.
  void OnStopSignal(::fit::closure on_stop);

 protected:
  // Constructor for testing when the component should run on a different loop than |loop_|.
  Component(async_dispatcher_t* dispatcher, std::unique_ptr<sys::ComponentContext> context);

 private:
  size_t InitialInstanceIndex() const;
  void WriteInstanceIndex() const;

  async::Loop loop_;
  async_dispatcher_t* dispatcher_;
  std::unique_ptr<sys::ComponentContext> context_;
  sys::ComponentInspector inspector_;
  size_t instance_index_;

  std::unique_ptr<fuchsia::process::lifecycle::Lifecycle> lifecycle_;
  std::unique_ptr<::fidl::Binding<fuchsia::process::lifecycle::Lifecycle>> lifecycle_connection_;
};

}  // namespace component
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_UTILS_COMPONENT_COMPONENT_H_
