// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SERVICE_LLCPP_SERVICE_HANDLER_H_
#define LIB_SERVICE_LLCPP_SERVICE_HANDLER_H_

#include <lib/fidl/llcpp/service_handler_interface.h>

#include <fbl/ref_ptr.h>
#include <fs/pseudo_dir.h>
#include <fs/service.h>

namespace llcpp::sys {

// A handler for an instance of a FIDL Service.
class ServiceHandler final : public ::fidl::ServiceHandlerInterface {
 public:
  ServiceHandler() = default;

  // Disable copying.
  ServiceHandler(const ServiceHandler&) = delete;
  ServiceHandler& operator=(const ServiceHandler&) = delete;

  // Enable moving.
  ServiceHandler(ServiceHandler&&) = default;
  ServiceHandler& operator=(ServiceHandler&&) = default;

  // Take the underlying pseudo-directory from the service handler.
  //
  // Once taken, the service handler is no longer safe to use.
  fbl::RefPtr<fs::PseudoDir> TakeDirectory() { return std::move(dir_); }

 private:
  // Add a |member| to the instance, whose connection will be handled by |handler|.
  //
  // # Errors
  //
  // ZX_ERR_ALREADY_EXISTS: The member already exists.
  ::zx::status<> AddAnyMember(cpp17::string_view member, AnyMemberHandler handler) override {
    // Bridge between fit::function and fbl::Function.
    auto bridge_func = [handler = std::move(handler)](::zx::channel request_channel) {
      return handler(std::move(request_channel)).status_value();
    };
    return ::zx::make_status(
        dir_->AddEntry(member, fbl::MakeRefCounted<fs::Service>(std::move(bridge_func))));
  }

  fbl::RefPtr<fs::PseudoDir> dir_ = fbl::MakeRefCounted<fs::PseudoDir>();
};

}  // namespace llcpp::sys

#endif  // LIB_SERVICE_LLCPP_SERVICE_HANDLER_H_
