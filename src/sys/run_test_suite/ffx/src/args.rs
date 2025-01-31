// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, ffx_core::ffx_command};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "test",
    description = "Run test suite",
    note = "Runs a test or suite implementing the `fuchsia.test.Suite` protocol.

Note that if running multiple iterations of a test and an iteration times
out, no further iterations will be executed."
)]

pub struct TestCommand {
    /// test timeout
    #[argh(option, short = 't')]
    pub timeout: Option<u32>,

    /// test url
    #[argh(positional)]
    pub test_url: String,

    /// a glob pattern for matching tests
    #[argh(option)]
    pub test_filter: Option<String>,

    #[argh(switch)]
    /// list tests in the suite
    pub list: bool,

    /// run tests that have been marked disabled/ignored
    #[argh(switch)]
    pub run_disabled: bool,

    /// run tests in parallel
    #[argh(option)]
    pub parallel: Option<u16>,

    /// number of times to run the test [default = 1]
    #[argh(option)]
    pub count: Option<u16>,
}
