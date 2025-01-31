// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fshost/fshost-boot-args.h"

#include <lib/fdio/directory.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/errors.h>

#include <fbl/string_printf.h>

namespace devmgr {

// static
std::shared_ptr<FshostBootArgs> FshostBootArgs::Create() {
  zx::channel remote, local;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    // This service might be missing if we're running in a test environment. Log
    // the error and continue.
    FX_LOGS(ERROR) << "failed to get boot arguments (" << zx_status_get_string(status)
                   << "), assuming test "
                      "environment and continuing";
    return std::make_shared<FshostBootArgs>(std::nullopt);
  }
  auto path = fbl::StringPrintf("/svc/%s", llcpp::fuchsia::boot::Arguments::Name);
  status = fdio_service_connect(path.data(), remote.release());
  if (status != ZX_OK) {
    // This service might be missing if we're running in a test environment. Log
    // the error and continue.
    FX_LOGS(ERROR) << "failed to get boot arguments (" << zx_status_get_string(status)
                   << "), assuming test "
                      "environment and continuing";
    return std::make_shared<FshostBootArgs>(std::nullopt);
  }
  return std::make_shared<FshostBootArgs>(
      llcpp::fuchsia::boot::Arguments::SyncClient(std::move(local)));
}

FshostBootArgs::FshostBootArgs(std::optional<llcpp::fuchsia::boot::Arguments::SyncClient> boot_args)
    : boot_args_(std::move(boot_args)) {
  if (!boot_args_) {
    return;
  }

  llcpp::fuchsia::boot::BoolPair defaults[] = {
      {fidl::StringView{"netsvc.netboot"}, netsvc_netboot_},
      {fidl::StringView{"zircon.system.disable-automount"}, zircon_system_disable_automount_},
      {fidl::StringView{"zircon.system.filesystem-check"}, zircon_system_filesystem_check_},
      {fidl::StringView{"zircon.system.wait-for-data"}, zircon_system_wait_for_data_},
  };
  auto ret = boot_args_->GetBools(fidl::unowned_vec(defaults));
  if (!ret.ok()) {
    FX_LOGS(ERROR) << "failed to get boolean parameters: " << ret.error() << "";
  } else {
    netsvc_netboot_ = ret->values[0];
    zircon_system_disable_automount_ = ret->values[1];
    zircon_system_filesystem_check_ = ret->values[2];
    zircon_system_wait_for_data_ = ret->values[3];
  }

  auto algorithm = GetStringArgument("blobfs.write-compression-algorithm");
  if (algorithm.is_error()) {
    if (algorithm.status_value() != ZX_ERR_NOT_FOUND) {
      FX_LOGS(ERROR) << "failed to get blobfs compression algorithm: " << algorithm.status_string();
    }
  } else {
    blobfs_write_compression_algorithm_ = std::move(algorithm).value();
  }

  auto eviction_policy = GetStringArgument("blobfs.cache-eviction-policy");
  if (eviction_policy.is_error()) {
    if (eviction_policy.status_value() != ZX_ERR_NOT_FOUND) {
      FX_LOGS(ERROR) << "failed to get blobfs eviction policy: " << eviction_policy.status_string();
    }
  } else {
    blobfs_eviction_policy_ = std::move(eviction_policy).value();
  }
}

zx::status<std::string> FshostBootArgs::GetStringArgument(std::string key) {
  if (!boot_args_) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }

  auto ret = boot_args_->GetString(fidl::unowned_str(key));
  if (!ret.ok()) {
    return zx::error(ret.status());
  }
  // fuchsia.boot.Arguments.GetString returns a "string?" value, so we need to check for null
  auto value = std::move(ret.value().value);
  if (value.is_null()) {
    return zx::error(ZX_ERR_NOT_FOUND);
  }
  return zx::ok(std::string(value.data(), value.size()));
}

zx::status<std::string> FshostBootArgs::pkgfs_file_with_path(std::string path) {
  return GetStringArgument(std::string("zircon.system.pkgfs.file.") + path);
}

zx::status<std::string> FshostBootArgs::pkgfs_cmd() {
  return GetStringArgument("zircon.system.pkgfs.cmd");
}

zx::status<std::string> FshostBootArgs::block_verity_seal() {
  return GetStringArgument("factory_verity_seal");
}

}  // namespace devmgr
