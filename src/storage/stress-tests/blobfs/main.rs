// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod blob;
mod blob_actor;
mod deletion_actor;
mod environment;
mod instance_actor;

use {
    crate::environment::BlobfsEnvironment,
    anyhow::Error,
    argh::FromArgs,
    fuchsia_async as fasync,
    log::{info, LevelFilter},
    stress_test::{run_test, StdoutLogger},
};

#[derive(Clone, Debug, FromArgs)]
/// Creates an instance of fvm and performs stressful operations on it
pub struct Args {
    /// seed to use for this stressor instance
    #[argh(option, short = 's')]
    seed: Option<u128>,

    /// number of operations to complete before exiting.
    #[argh(option, short = 'o')]
    num_operations: Option<u64>,

    /// filter logging by level (off, error, warn, info, debug, trace)
    #[argh(option, short = 'l')]
    log_filter: Option<LevelFilter>,

    /// size of one block of the ramdisk (in bytes)
    #[argh(option, default = "512")]
    ramdisk_block_size: u64,

    /// number of blocks in the ramdisk
    /// defaults to 106MiB ramdisk
    #[argh(option, default = "217088")]
    ramdisk_block_count: u64,

    /// size of one slice in FVM (in bytes)
    #[argh(option, default = "32768")]
    fvm_slice_size: u64,

    /// controls how often blobfs is disconnected by
    /// crashing the block device.
    #[argh(option, short = 'd')]
    disconnect_secs: Option<u64>,

    /// if set, the test runs for this time limit before exiting successfully.
    #[argh(option, short = 't')]
    time_limit_secs: Option<u64>,
}

// The path to the blobfs filesystem in the test's namespace
pub const BLOBFS_MOUNT_PATH: &str = "/blobfs";

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    // Get arguments from command line
    let args: Args = argh::from_env();

    // Initialize logging
    StdoutLogger::init(args.log_filter.unwrap_or(LevelFilter::Info));

    // Setup the blobfs environment
    let env = BlobfsEnvironment::new(args).await;

    // Run the test
    run_test(env).await;

    info!("Stress test is exiting successfully!");

    Ok(())
}
