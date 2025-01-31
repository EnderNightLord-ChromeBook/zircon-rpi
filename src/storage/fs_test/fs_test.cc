// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fs_test/fs_test.h"

#include <errno.h>
#include <fuchsia/device/llcpp/fidl.h>
#include <fuchsia/fs/cpp/fidl.h>
#include <fuchsia/hardware/nand/c/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/namespace.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/memfs/memfs.h>
#include <lib/sync/completion.h>
#include <lib/zx/channel.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zircon/errors.h>

#include <iostream>
#include <utility>

#include <fbl/unique_fd.h>
#include <fs-management/admin.h>
#include <fs-management/format.h>
#include <fs-management/launch.h>
#include <fs-management/mount.h>
#include <fs/vfs.h>

#include "src/lib/isolated_devmgr/v2_component/bind_devfs_to_namespace.h"
#include "src/lib/isolated_devmgr/v2_component/fvm.h"
#include "src/storage/blobfs/blob_layout.h"
#include "src/storage/fs_test/blobfs_test.h"
#include "src/storage/fs_test/minfs_test.h"

namespace fs_test {
namespace {

std::string StripTrailingSlash(const std::string& in) {
  if (!in.empty() && in.back() == '/') {
    return in.substr(0, in.length() - 1);
  } else {
    return in;
  }
}

// Creates a ram-disk with an optional FVM partition. Returns the ram-disk and the device path.
zx::status<std::pair<isolated_devmgr::RamDisk, std::string>> CreateRamDisk(
    const TestFilesystemOptions& options) {
  if (options.use_ram_nand) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }
  zx::vmo vmo;
  fzl::VmoMapper mapper;
  auto status =
      zx::make_status(mapper.CreateAndMap(options.device_block_size * options.device_block_count,
                                          ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, nullptr, &vmo));
  if (status.is_error()) {
    std::cout << "Unable to create VMO for ramdisk: " << status.status_string() << std::endl;
    return status.take_error();
  }

  // Fill the ram-disk with a non-zero value so that we don't inadvertently depend on it being
  // zero filled.
  if (!options.zero_fill) {
    memset(mapper.start(), 0xaf, mapper.size());
  }

  // Create a ram-disk.
  auto ram_disk_or =
      isolated_devmgr::RamDisk::CreateWithVmo(std::move(vmo), options.device_block_size);
  if (ram_disk_or.is_error()) {
    return ram_disk_or.take_error();
  }

  // Create an FVM partition if requested.
  std::string device_path;
  if (options.use_fvm) {
    auto fvm_partition_or =
        isolated_devmgr::CreateFvmPartition(ram_disk_or.value().path(), options.fvm_slice_size);
    if (fvm_partition_or.is_error()) {
      return fvm_partition_or.take_error();
    }
    device_path = fvm_partition_or.value();
  } else {
    device_path = ram_disk_or.value().path();
  }

  return zx::ok(std::make_pair(std::move(ram_disk_or).value(), device_path));
}

// Creates a ram-nand device.  It does not create an FVM partition; that is left to the caller.
zx::status<std::pair<ramdevice_client::RamNand, std::string>> CreateRamNand(
    const TestFilesystemOptions& options) {
  auto status = isolated_devmgr::OneTimeSetUp();
  if (status.is_error()) {
    return status.take_error();
  }

  constexpr int kPageSize = 4096;
  constexpr int kPagesPerBlock = 64;
  constexpr int kOobSize = 8;

  uint32_t block_count;
  zx::vmo vmo;
  if (options.ram_nand_vmo->is_valid()) {
    uint64_t vmo_size;
    status = zx::make_status(options.ram_nand_vmo->get_size(&vmo_size));
    if (status.is_error()) {
      return status.take_error();
    }
    block_count = vmo_size / (kPageSize + kOobSize) / kPagesPerBlock;
    // For now, when using a ram-nand device, the only supported device block size is 8 KiB, so
    // raise an error if the user tries to ask for something different.
    if ((options.device_block_size != 0 && options.device_block_size != 8192) ||
        (options.device_block_count != 0 &&
         options.device_block_size * options.device_block_count !=
             block_count * kPageSize * kPagesPerBlock)) {
      std::cout << "Bad device parameters" << std::endl;
      return zx::error(ZX_ERR_INVALID_ARGS);
    }
    status =
        zx::make_status(options.ram_nand_vmo->create_child(ZX_VMO_CHILD_SLICE, 0, vmo_size, &vmo));
    if (status.is_error()) {
      return status.take_error();
    }
  } else if (options.device_block_size != 8192) {  // FTL exports a device with 8 KiB blocks.
    return zx::error(ZX_ERR_INVALID_ARGS);
  } else {
    block_count =
        options.device_block_size * options.device_block_count / kPageSize / kPagesPerBlock;
  }

  status = zx::make_status(wait_for_device("/dev/misc/nand-ctl", zx::sec(10).get()));
  if (status.is_error()) {
    std::cout << "Failed waiting for /dev/misc/nand-ctl to appear: " << status.status_string()
              << std::endl;
    return status.take_error();
  }

  std::optional<ramdevice_client::RamNand> ram_nand;
  fuchsia_hardware_nand_RamNandInfo config = {
      .vmo = vmo.release(),
      .nand_info.page_size = kPageSize,
      .nand_info.pages_per_block = kPagesPerBlock,
      .nand_info.num_blocks = block_count,
      .nand_info.ecc_bits = 8,
      .nand_info.oob_size = kOobSize,
      .nand_info.nand_class = fuchsia_hardware_nand_Class_FTL};
  status = zx::make_status(ramdevice_client::RamNand::Create(&config, &ram_nand));
  if (status.is_error()) {
    std::cout << "RamNand::Create failed: " << status.status_string() << std::endl;
    return status.take_error();
  }

  std::string ftl_path = std::string(ram_nand->path()) + "/ftl/block";
  status = zx::make_status(wait_for_device(ftl_path.c_str(), zx::sec(10).get()));
  if (status.is_error()) {
    std::cout << "Timed out waiting for RamNand" << std::endl;
    return status.take_error();
  }
  return zx::ok(std::make_pair(*std::move(ram_nand), std::move(ftl_path)));
}

using RamDevice = std::variant<isolated_devmgr::RamDisk, ramdevice_client::RamNand>;

// Returns device and device path.
zx::status<std::pair<RamDevice, std::string>> CreateRamDevice(
    const TestFilesystemOptions& options) {
  if (options.use_ram_nand) {
    auto ram_nand_or = CreateRamNand(options);
    if (ram_nand_or.is_error()) {
      return ram_nand_or.take_error();
    }
    auto [ram_nand, nand_device_path] = std::move(ram_nand_or).value();

    auto fvm_partition_or =
        isolated_devmgr::CreateFvmPartition(nand_device_path, options.fvm_slice_size);
    if (fvm_partition_or.is_error()) {
      std::cout << "Failed to create FVM partition: " << fvm_partition_or.status_string()
                << std::endl;
      return fvm_partition_or.take_error();
    }

    return zx::ok(std::make_pair(std::move(ram_nand), std::move(fvm_partition_or).value()));
  } else {
    auto ram_disk_or = CreateRamDisk(options);
    if (ram_disk_or.is_error()) {
      return ram_disk_or.take_error();
    }
    auto [device, device_path] = std::move(ram_disk_or).value();
    return zx::ok(std::make_pair(std::move(device), std::move(device_path)));
  }
}

// Returns device and device path.
zx::status<std::pair<ramdevice_client::RamNand, std::string>> OpenRamNand(
    const TestFilesystemOptions& options) {
  if (!options.use_ram_nand || !options.ram_nand_vmo->is_valid()) {
    return zx::error(ZX_ERR_NOT_SUPPORTED);
  }

  // First create the ram-nand device.
  auto ram_nand_or = CreateRamNand(options);
  if (ram_nand_or.is_error()) {
    return ram_nand_or.take_error();
  }
  auto [ram_nand, ftl_device_path] = std::move(ram_nand_or).value();

  // Now bind FVM to it.
  fbl::unique_fd ftl_device(open(ftl_device_path.c_str(), O_RDWR));
  if (!ftl_device)
    return zx::error(ZX_ERR_BAD_STATE);
  auto status = isolated_devmgr::BindFvm(ftl_device.get());
  if (status.is_error()) {
    std::cout << "Unable to bind FVM: " << status.status_string() << std::endl;
    return status.take_error();
  }

  // Wait for the partition to show up.
  std::string device_path = ftl_device_path + "/fvm/fs-test-partition-p-1/block";
  status = zx::make_status(wait_for_device(device_path.c_str(), zx::sec(10).get()));
  if (status.is_error()) {
    std::cout << "Timed out waiting for partition to show up" << std::endl;
    return status.take_error();
  }

  return zx::ok(std::make_pair(std::move(ram_nand), std::move(device_path)));
}

// A wrapper around fs-management that can be used by filesytems if they so wish.
zx::status<> FsMount(const std::string& device_path, const std::string& mount_path,
                     disk_format_t format, const mount_options_t& mount_options,
                     zx::channel* outgoing_directory = nullptr) {
  auto fd = fbl::unique_fd(open(device_path.c_str(), O_RDWR));
  if (!fd) {
    std::cout << "Could not open device: " << device_path << ": errno=" << errno << std::endl;
    return zx::error(ZX_ERR_BAD_STATE);
  }

  mount_options_t options = mount_options;
  options.register_fs = false;
  options.bind_to_namespace = true;
  if (outgoing_directory) {
    zx::channel server;
    auto status = zx::make_status(zx::channel::create(0, outgoing_directory, &server));
    if (status.is_error()) {
      std::cout << "Unable to create channel for outgoing directory: " << status.status_string()
                << std::endl;
      return status;
    }
    options.outgoing_directory.client = outgoing_directory->get();
    options.outgoing_directory.server = server.release();
  }

  // Uncomment the following line to force an fsck at the end of every transaction (where
  // supported).
  // options.fsck_after_every_transaction = true;

  // |fd| is consumed by mount.
  auto status = zx::make_status(mount(fd.release(), StripTrailingSlash(mount_path).c_str(), format,
                                      &options, launch_stdio_async));
  if (status.is_error()) {
    std::cout << "Could not mount " << disk_format_string(format)
              << " file system: " << status.status_string() << std::endl;
    return status;
  }
  return zx::ok();
}

zx::status<> FsUnbind(const std::string& mount_path) {
  fdio_ns_t* ns;
  if (auto status = zx::make_status(fdio_ns_get_installed(&ns)); status.is_error()) {
    return status;
  }
  if (auto status = zx::make_status(fdio_ns_unbind(ns, StripTrailingSlash(mount_path).c_str()));
      status.is_error()) {
    std::cout << "Unable to unbind: " << status.status_string() << std::endl;
    return status;
  }
  return zx::ok();
}

zx::status<> FsDirectoryAdminUnmount(const std::string& mount_path) {
  // O_ADMIN is not part of the SDK.  Eventually, this should switch to using fs.Admin.
  constexpr int kAdmin = 0x0000'0004;
  int fd = open(mount_path.c_str(), O_DIRECTORY | kAdmin);
  if (fd < 0) {
    std::cout << "Unable to open mount point: " << strerror(errno) << std::endl;
    return zx::error(ZX_ERR_INTERNAL);
  }
  zx_handle_t handle;
  if (auto status = zx::make_status(fdio_get_service_handle(fd, &handle)); status.is_error()) {
    std::cout << "Unable to get service handle: " << status.status_string() << std::endl;
    return status;
  }
  if (auto status =
          zx::make_status(fs::Vfs::UnmountHandle(zx::channel(handle), zx::time::infinite()));
      status.is_error()) {
    std::cout << "Unable to unmount: " << status.status_string() << std::endl;
    return status;
  }
  return zx::ok();
}

zx::status<> DefaultFormat(const std::string& device_path, disk_format_t format,
                           const mkfs_options_t& options) {
  auto status = zx::make_status(mkfs(device_path.c_str(), format, launch_stdio_sync, &options));
  if (status.is_error()) {
    std::cout << "Could not format " << disk_format_string(format)
              << " file system: " << status.status_string() << std::endl;
    return status;
  }
  return zx::ok();
}

}  // namespace

TestFilesystemOptions TestFilesystemOptions::DefaultMinfs() {
  return TestFilesystemOptions{.description = "MinfsWithFvm",
                               .use_fvm = true,
                               .device_block_size = 512,
                               .device_block_count = 131'072,
                               .fvm_slice_size = 32'768,
                               .filesystem = &MinfsFilesystem::SharedInstance()};
}

TestFilesystemOptions TestFilesystemOptions::MinfsWithoutFvm() {
  TestFilesystemOptions minfs_with_no_fvm = TestFilesystemOptions::DefaultMinfs();
  minfs_with_no_fvm.description = "MinfsWithoutFvm";
  minfs_with_no_fvm.use_fvm = false;
  return minfs_with_no_fvm;
}

TestFilesystemOptions TestFilesystemOptions::DefaultMemfs() {
  return TestFilesystemOptions{.description = "Memfs",
                               .filesystem = &MemfsFilesystem::SharedInstance()};
}

TestFilesystemOptions TestFilesystemOptions::DefaultFatfs() {
  return TestFilesystemOptions{.description = "Fatfs",
                               .use_fvm = false,
                               .device_block_size = 512,
                               .device_block_count = 196'608,
                               .filesystem = &FatFilesystem::SharedInstance()};
}

TestFilesystemOptions TestFilesystemOptions::DefaultBlobfs() {
  return TestFilesystemOptions{.description = "Blobfs",
                               .use_fvm = true,
                               .device_block_size = 512,
                               .device_block_count = 196'608,
                               .fvm_slice_size = 32'768,
                               .filesystem = &BlobfsFilesystem::SharedInstance()};
}

TestFilesystemOptions TestFilesystemOptions::BlobfsWithoutFvm() {
  TestFilesystemOptions blobfs_with_no_fvm = TestFilesystemOptions::DefaultBlobfs();
  blobfs_with_no_fvm.description = "BlobfsWithoutFvm";
  blobfs_with_no_fvm.use_fvm = false;
  return blobfs_with_no_fvm;
}

std::ostream& operator<<(std::ostream& out, const TestFilesystemOptions& options) {
  return out << options.description;
}

std::vector<TestFilesystemOptions> AllTestMinfs() {
  return std::vector<TestFilesystemOptions>{TestFilesystemOptions::DefaultMinfs(),
                                            TestFilesystemOptions::MinfsWithoutFvm()};
}

// Note: blobfs is intentionally absent, since it is not intended to run as part of the
// fs_test suite.
std::vector<TestFilesystemOptions> AllTestFilesystems() {
  return std::vector<TestFilesystemOptions>{
      TestFilesystemOptions::DefaultMinfs(), TestFilesystemOptions::MinfsWithoutFvm(),
      TestFilesystemOptions::DefaultMemfs(), TestFilesystemOptions::DefaultFatfs()};
}

std::vector<TestFilesystemOptions> MapAndFilterAllTestFilesystems(
    std::function<std::optional<TestFilesystemOptions>(const TestFilesystemOptions&)>
        map_and_filter) {
  std::vector<TestFilesystemOptions> results;
  for (const TestFilesystemOptions& options : AllTestFilesystems()) {
    auto r = map_and_filter(options);
    if (r) {
      results.push_back(*std::move(r));
    }
  }
  return results;
}

// -- FilesystemInstance --

// Default implementation
zx::status<> FilesystemInstance::Unmount(const std::string& mount_path) {
  if (auto status = FsDirectoryAdminUnmount(mount_path); status.is_error()) {
    return status;
  }
  return FsUnbind(mount_path);
}

template <typename T, typename Instance>
zx::status<std::unique_ptr<FilesystemInstance>> FilesystemImplWithDefaultMake<T, Instance>::Make(
    const TestFilesystemOptions& options) const {
  auto result = CreateRamDevice(options);
  if (result.is_error()) {
    return result.take_error();
  }
  auto [device, device_path] = std::move(result).value();
  auto instance = std::make_unique<Instance>(std::move(device), std::move(device_path));
  zx::status<> status = instance->Format(options);
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(instance));
}

// -- Minfs --

class MinfsInstance : public FilesystemInstance {
 public:
  MinfsInstance(RamDevice device, std::string device_path)
      : device_(std::move(device)), device_path_(std::move(device_path)) {}

  virtual zx::status<> Format(const TestFilesystemOptions& options) override {
    return DefaultFormat(device_path_, DISK_FORMAT_MINFS, default_mkfs_options);
  }

  zx::status<> Mount(const std::string& mount_path, const mount_options_t& options) override {
    return FsMount(device_path_, mount_path, DISK_FORMAT_MINFS, options);
  }

  zx::status<> Fsck() override {
    fsck_options_t options{
        .verbose = false,
        .never_modify = true,
        .always_modify = false,
        .force = true,
    };
    return zx::make_status(
        fsck(device_path_.c_str(), DISK_FORMAT_MINFS, &options, launch_stdio_sync));
  }

  zx::status<std::string> DevicePath() const override { return zx::ok(std::string(device_path_)); }

  isolated_devmgr::RamDisk* GetRamDisk() override {
    return std::get_if<isolated_devmgr::RamDisk>(&device_);
  }

  ramdevice_client::RamNand* GetRamNand() override {
    return std::get_if<ramdevice_client::RamNand>(&device_);
  }

 private:
  RamDevice device_;
  std::string device_path_;
};

zx::status<std::unique_ptr<FilesystemInstance>> MinfsFilesystem::Open(
    const TestFilesystemOptions& options) const {
  auto result = OpenRamNand(options);
  if (result.is_error()) {
    return result.take_error();
  }
  auto [ram_nand, device_path] = std::move(result).value();
  return zx::ok(std::make_unique<MinfsInstance>(std::move(ram_nand), std::move(device_path)));
}

// -- Memfs --

class MemfsInstance : public FilesystemInstance {
 public:
  MemfsInstance() : loop_(&kAsyncLoopConfigNeverAttachToThread) {
    ZX_ASSERT(loop_.StartThread() == ZX_OK);
  }
  ~MemfsInstance() override {
    if (fs_) {
      sync_completion_t sync;
      memfs_free_filesystem(fs_, &sync);
      ZX_ASSERT(sync_completion_wait(&sync, zx::duration::infinite().get()) == ZX_OK);
    }
  }
  zx::status<> Format(const TestFilesystemOptions&) override {
    return zx::make_status(
        memfs_create_filesystem(loop_.dispatcher(), &fs_, root_.reset_and_get_address()));
  }

  zx::status<> Mount(const std::string& mount_path, const mount_options_t& options) override {
    if (!root_) {
      // Already mounted.
      return zx::error(ZX_ERR_BAD_STATE);
    }
    fdio_ns_t* ns;
    if (auto status = zx::make_status(fdio_ns_get_installed(&ns)); status.is_error()) {
      return status;
    }
    return zx::make_status(
        fdio_ns_bind(ns, StripTrailingSlash(mount_path).c_str(), root_.release()));
  }

  zx::status<> Unmount(const std::string& mount_path) override { return FsUnbind(mount_path); }

  zx::status<> Fsck() override { return zx::ok(); }

  zx::status<std::string> DevicePath() const override { return zx::error(ZX_ERR_BAD_STATE); }

 private:
  async::Loop loop_;
  memfs_filesystem_t* fs_ = nullptr;
  zx::channel root_;  // Not valid after mounted.
};

zx::status<std::unique_ptr<FilesystemInstance>> MemfsFilesystem::Make(
    const TestFilesystemOptions& options) const {
  auto instance = std::make_unique<MemfsInstance>();
  zx::status<> status = instance->Format(options);
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(instance));
}

// -- Fatfs --

class FatfsInstance : public FilesystemInstance {
 public:
  FatfsInstance(isolated_devmgr::RamDisk ram_disk, std::string device_path)
      : ram_disk_(std::move(ram_disk)), device_path_(std::move(device_path)) {}

  zx::status<> Format(const TestFilesystemOptions&) override {
    mkfs_options_t mkfs_options = default_mkfs_options;
    mkfs_options.sectors_per_cluster = 2;  // 1 KiB cluster size
    return DefaultFormat(device_path_, DISK_FORMAT_FAT, mkfs_options);
  }

  zx::status<> Mount(const std::string& mount_path, const mount_options_t& base_options) override {
    mount_options_t options = base_options;
    // Fatfs doesn't support DirectoryAdmin.
    options.admin = false;
    return FsMount(device_path_, mount_path, DISK_FORMAT_FAT, options, &outgoing_directory_);
  }

  zx::status<> Unmount(const std::string& mount_path) override {
    // Detach from the namespace.
    if (auto status = FsUnbind(mount_path); status.is_error()) {
      return status;
    }

    // Now shut down the filesystem.
    fidl::SynchronousInterfacePtr<fuchsia::fs::Admin> admin;
    std::string service_name = std::string("svc/") + fuchsia::fs::Admin::Name_;
    auto status = zx::make_status(fdio_service_connect_at(
        outgoing_directory_.get(), service_name.c_str(), admin.NewRequest().TakeChannel().get()));
    if (status.is_error()) {
      std::cout << "Unable to connect to admin service: " << status.status_string() << std::endl;
      return status;
    }
    status = zx::make_status(admin->Shutdown());
    if (status.is_error()) {
      std::cout << "Shut down failed: " << status.status_string() << std::endl;
      return status;
    }
    outgoing_directory_.reset();
    return zx::ok();
  }

  zx::status<> Fsck() override {
    fsck_options_t options{
        .verbose = false,
        .never_modify = true,
        .always_modify = false,
        .force = true,
    };
    return zx::make_status(
        fsck(device_path_.c_str(), DISK_FORMAT_FAT, &options, launch_stdio_sync));
  }

  zx::status<std::string> DevicePath() const override { return zx::ok(std::string(device_path_)); }
  zx::unowned_channel GetOutgoingDirectory() const override { return outgoing_directory_.borrow(); }

 private:
  isolated_devmgr::RamDisk ram_disk_;
  std::string device_path_;
  zx::channel outgoing_directory_;
};

zx::status<std::unique_ptr<FilesystemInstance>> FatFilesystem::Make(
    const TestFilesystemOptions& options) const {
  auto ram_disk_or = CreateRamDisk(options);
  if (ram_disk_or.is_error()) {
    return ram_disk_or.take_error();
  }
  auto [ram_disk, device_path] = std::move(ram_disk_or).value();
  auto instance = std::make_unique<FatfsInstance>(std::move(ram_disk), device_path);
  zx::status<> status = instance->Format(options);
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(instance));
}

// -- Blobfs --

class BlobfsInstance : public FilesystemInstance {
 public:
  BlobfsInstance(RamDevice device, std::string device_path)
      : device_(std::move(device)), device_path_(std::move(device_path)) {}

  zx::status<> Format(const TestFilesystemOptions& options) override {
    mkfs_options_t mkfs_options = default_mkfs_options;
    if (options.blob_layout_format) {
      mkfs_options.blob_layout_format =
          blobfs::GetBlobLayoutFormatCommandLineArg(options.blob_layout_format.value());
    }
    return DefaultFormat(device_path_, DISK_FORMAT_BLOBFS, mkfs_options);
  }

  zx::status<> Mount(const std::string& mount_path, const mount_options_t& options) override {
    return FsMount(device_path_, mount_path, DISK_FORMAT_BLOBFS, options, &outgoing_directory_);
  }

  zx::status<> Unmount(const std::string& mount_path) override {
    outgoing_directory_.reset();
    return FilesystemInstance::Unmount(mount_path);
  }

  zx::status<> Fsck() override {
    fsck_options_t options{
        .verbose = false,
        .never_modify = true,
        .always_modify = false,
        .force = true,
    };
    return zx::make_status(
        fsck(device_path_.c_str(), DISK_FORMAT_BLOBFS, &options, launch_stdio_sync));
  }

  zx::status<std::string> DevicePath() const override { return zx::ok(std::string(device_path_)); }
  isolated_devmgr::RamDisk* GetRamDisk() override {
    return std::get_if<isolated_devmgr::RamDisk>(&device_);
  }
  ramdevice_client::RamNand* GetRamNand() override {
    return std::get_if<ramdevice_client::RamNand>(&device_);
  }
  zx::unowned_channel GetOutgoingDirectory() const override { return outgoing_directory_.borrow(); }

 private:
  RamDevice device_;
  std::string device_path_;
  zx::channel outgoing_directory_;
};

zx::status<std::unique_ptr<FilesystemInstance>> BlobfsFilesystem::Open(
    const TestFilesystemOptions& options) const {
  auto result = OpenRamNand(options);
  if (result.is_error()) {
    return result.take_error();
  }
  auto [ram_nand, device_path] = std::move(result).value();
  return zx::ok(std::make_unique<BlobfsInstance>(std::move(ram_nand), std::move(device_path)));
}

// --

zx::status<TestFilesystem> TestFilesystem::FromInstance(
    const TestFilesystemOptions& options, std::unique_ptr<FilesystemInstance> instance) {
  static uint32_t mount_index;
  TestFilesystem filesystem(options, std::move(instance),
                            std::string("/fs_test." + std::to_string(mount_index++) + "/"));
  auto status = filesystem.Mount();
  if (status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(filesystem));
}

zx::status<TestFilesystem> TestFilesystem::Create(const TestFilesystemOptions& options) {
  auto instance_or = options.filesystem->Make(options);
  if (instance_or.is_error()) {
    return instance_or.take_error();
  }
  return FromInstance(options, std::move(instance_or).value());
}

zx::status<TestFilesystem> TestFilesystem::Open(const TestFilesystemOptions& options) {
  auto instance_or = options.filesystem->Open(options);
  if (instance_or.is_error()) {
    return instance_or.take_error();
  }
  return FromInstance(options, std::move(instance_or).value());
}

TestFilesystem::~TestFilesystem() {
  if (filesystem_) {
    if (mounted_) {
      auto status = Unmount();
      if (status.is_error()) {
        std::cout << "warning: failed to unmount: " << status.status_string() << std::endl;
      }
    }
    rmdir(mount_path_.c_str());
  }
}

zx::status<> TestFilesystem::MountWithOptions(const mount_options_t& options) {
  auto status = filesystem_->Mount(mount_path_, options);
  if (status.is_ok()) {
    mounted_ = true;
  }
  return status;
}

zx::status<> TestFilesystem::Unmount() {
  if (!filesystem_) {
    return zx::ok();
  }
  auto status = filesystem_->Unmount(mount_path_);
  if (status.is_ok()) {
    mounted_ = false;
  }
  return status;
}

zx::status<> TestFilesystem::Fsck() { return filesystem_->Fsck(); }

zx::status<std::string> TestFilesystem::DevicePath() const { return filesystem_->DevicePath(); }

zx::status<llcpp::fuchsia::io::FilesystemInfo> TestFilesystem::GetFsInfo() {
  fbl::unique_fd fd(open(mount_path().c_str(), O_RDONLY | O_DIRECTORY));
  if (!fd) {
    return zx::error(ZX_ERR_BAD_STATE);
  }

  fdio_cpp::FdioCaller caller(std::move(fd));
  auto result = llcpp::fuchsia::io::DirectoryAdmin::Call::QueryFilesystem(
      zx::unowned_channel(caller.borrow_channel()));
  if (result.status() != ZX_OK) {
    return zx::error(result.status());
  }
  if (result.value().s != ZX_OK) {
    return zx::error(result.value().s);
  }
  return zx::ok(*result->info);
}

}  // namespace fs_test
