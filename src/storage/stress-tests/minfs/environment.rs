// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        deletion_actor::DeletionActor, file_actor::FileActor, instance_actor::InstanceActor, Args,
    },
    async_trait::async_trait,
    fidl_fuchsia_hardware_block_partition::Guid,
    fidl_fuchsia_io::{OPEN_RIGHT_READABLE, OPEN_RIGHT_WRITABLE},
    fs_management::Minfs,
    fuchsia_zircon::Vmo,
    futures::lock::Mutex,
    rand::{rngs::SmallRng, Rng, SeedableRng},
    std::path::PathBuf,
    std::sync::Arc,
    storage_stress_test_utils::{
        fvm::{get_volume_path, FvmInstance},
        io::Directory,
    },
    stress_test::{actor::ActorRunner, environment::Environment, random_seed},
};

// All partitions in this test have their type set to this arbitrary GUID.
const TYPE_GUID: Guid = Guid {
    value: [0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf],
};

// The path to the minfs filesystem in the test's namespace
const MINFS_MOUNT_PATH: &str = "/minfs";

pub fn open_dir_at_minfs_root(subdir: &str) -> Directory {
    let path = PathBuf::from(MINFS_MOUNT_PATH).join(subdir);
    Directory::from_namespace(path, OPEN_RIGHT_WRITABLE | OPEN_RIGHT_READABLE).unwrap()
}

/// Describes the environment that this minfs stress test will run under.
pub struct MinfsEnvironment {
    seed: u128,
    args: Args,
    vmo: Vmo,
    volume_guid: Guid,
    instance_actor: Arc<Mutex<InstanceActor>>,
    file_actor: Arc<Mutex<FileActor>>,
    deletion_actor: Arc<Mutex<DeletionActor>>,
}

impl MinfsEnvironment {
    pub async fn new(args: Args) -> Self {
        // Create the VMO that the ramdisk is backed by
        let vmo_size = args.ramdisk_block_count * args.ramdisk_block_size;
        let vmo = Vmo::create(vmo_size).unwrap();

        // Initialize the VMO with FVM partition style and a single minfs partition

        // Create a ramdisk and setup FVM.
        let mut fvm =
            FvmInstance::new(true, &vmo, args.fvm_slice_size, args.ramdisk_block_size).await;

        // Create a minfs volume
        let volume_guid = fvm.new_volume("minfs", TYPE_GUID).await;

        // Find the path to the volume
        let block_path = fvm.block_path();
        let volume_path = get_volume_path(block_path, &volume_guid).await;

        // Initialize minfs on volume
        let mut minfs = Minfs::new(volume_path.to_str().unwrap()).unwrap();
        minfs.format().unwrap();
        minfs.mount(MINFS_MOUNT_PATH).unwrap();

        let seed = match args.seed {
            Some(seed) => seed,
            None => random_seed(),
        };

        let mut rng = SmallRng::from_seed(seed.to_le_bytes());

        // Make a home directory for file actor and deletion actor
        let root_dir =
            Directory::from_namespace(MINFS_MOUNT_PATH, OPEN_RIGHT_WRITABLE | OPEN_RIGHT_READABLE)
                .unwrap();
        root_dir
            .create_directory("home1", OPEN_RIGHT_WRITABLE | OPEN_RIGHT_READABLE)
            .await
            .unwrap();

        let file_actor = {
            let rng = SmallRng::from_seed(rng.gen());
            let home_dir = open_dir_at_minfs_root("home1");
            Arc::new(Mutex::new(FileActor::new(rng, home_dir)))
        };
        let deletion_actor = {
            let rng = SmallRng::from_seed(rng.gen());
            let home_dir = open_dir_at_minfs_root("home1");
            Arc::new(Mutex::new(DeletionActor::new(rng, home_dir)))
        };

        let instance_actor = Arc::new(Mutex::new(InstanceActor { fvm, minfs }));

        Self { seed, args, vmo, volume_guid, file_actor, deletion_actor, instance_actor }
    }
}

impl std::fmt::Debug for MinfsEnvironment {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Environment").field("seed", &self.seed).field("args", &self.args).finish()
    }
}

#[async_trait]
impl Environment for MinfsEnvironment {
    fn target_operations(&self) -> Option<u64> {
        self.args.num_operations
    }

    fn timeout_seconds(&self) -> Option<u64> {
        self.args.time_limit_secs
    }

    fn actor_runners(&mut self) -> Vec<ActorRunner> {
        let mut runners = vec![
            ActorRunner::new("file_actor", 0, self.file_actor.clone()),
            ActorRunner::new("deletion_actor", 5, self.deletion_actor.clone()),
        ];

        if let Some(secs) = self.args.disconnect_secs {
            if secs > 0 {
                let runner = ActorRunner::new("instance_actor", secs, self.instance_actor.clone());
                runners.push(runner);
            }
        }

        runners
    }

    async fn reset(&mut self) {
        {
            let mut actor = self.instance_actor.lock().await;

            // Kill the minfs process in case it was still running
            let _ = actor.minfs.kill();

            // Create a ramdisk and setup FVM.
            let fvm = FvmInstance::new(
                false,
                &self.vmo,
                self.args.fvm_slice_size,
                self.args.ramdisk_block_size,
            )
            .await;

            // Find the path to the volume
            let block_path = fvm.block_path();
            let volume_path = get_volume_path(block_path, &self.volume_guid).await;

            // Initialize minfs on volume
            let mut minfs = Minfs::new(volume_path.to_str().unwrap()).unwrap();

            // Mount the minfs volume
            minfs.fsck().unwrap();
            minfs.mount(MINFS_MOUNT_PATH).unwrap();

            // Replace the fvm and minfs instances
            actor.fvm = fvm;
            actor.minfs = minfs;
        }

        {
            // Replace the root directory with a new one
            let mut actor = self.file_actor.lock().await;
            actor.home_dir = open_dir_at_minfs_root("home1");
        }

        {
            // Replace the root directory with a new one
            let mut actor = self.deletion_actor.lock().await;
            actor.home_dir = open_dir_at_minfs_root("home1");
        }
    }
}
