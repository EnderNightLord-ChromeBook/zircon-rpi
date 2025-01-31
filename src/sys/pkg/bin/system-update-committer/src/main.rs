// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{fidl::FidlServer, metadata::put_metadata_in_happy_state, reboot::wait_and_reboot},
    anyhow::{anyhow, Context, Error},
    config::Config,
    fidl_fuchsia_hardware_power_statecontrol::AdminMarker as PowerStateControlMarker,
    fidl_fuchsia_paver::PaverMarker,
    fidl_fuchsia_update_verify::BlobfsVerifierMarker,
    fuchsia_async as fasync,
    fuchsia_component::{client::connect_to_service, server::ServiceFs},
    fuchsia_inspect::{self as finspect, health::Reporter},
    fuchsia_syslog::{fx_log_info, fx_log_warn},
    fuchsia_zircon::{self as zx, HandleBased},
    futures::{channel::oneshot, prelude::*, stream::FuturesUnordered},
    std::{sync::Arc, time::Duration},
};

mod config;
mod fidl;
mod metadata;
mod reboot;

// The system persists the reboot reason via a component called last_reboot. If we issue a reboot
// before last_reboot starts, the reboot reason will not persist. Since last_reboot is v1 and the
// system-update-committer is v2, and v2 components start earlier than v1 components, let's try to
// minimize this risk by defining a minimum duration we must wait to issue a reboot. Ideally, reboot
// clients should not have to worry about this. However, given the transition from v1 to v2, for now
// we mitigate this with a timer. This value was determined experimentally on Astro, where there
// seems to be a ~2 second gap between the system-update-committer and last_reboot starting.
const MINIMUM_REBOOT_WAIT: Duration = Duration::from_secs(5);

pub fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["system-update-committer"])
        .context("while initializing logger")?;
    fx_log_info!("starting system-update-committer");

    let mut executor = fasync::Executor::new().context("error creating executor")?;
    let () = executor.run_singlethreaded(main_inner_async()).map_err(|err| {
        // Use anyhow to print the error chain.
        let err = anyhow!(err);
        fuchsia_syslog::fx_log_err!("error running system-update-committer: {:#}", err);
        err
    })?;

    fx_log_info!("shutting down system-update-committer");
    Ok(())
}

async fn main_inner_async() -> Result<(), Error> {
    let inspector = finspect::Inspector::new();
    let verification_node = inspector.root().create_child("verification");
    let mut health_node = finspect::health::Node::new(inspector.root());

    let verification_node_ref = &verification_node;
    let health_node_ref = &mut health_node;

    let config = Config::load_from_config_data_or_default();
    let reboot_timer = fasync::Timer::new(MINIMUM_REBOOT_WAIT);

    let paver = connect_to_service::<PaverMarker>().context("while connecting to paver")?;
    let (boot_manager, boot_manager_server_end) =
        ::fidl::endpoints::create_proxy().context("while creating BootManager endpoints")?;

    paver
        .find_boot_manager(boot_manager_server_end)
        .context("transport error while calling find_boot_manager()")?;

    let reboot_proxy = connect_to_service::<PowerStateControlMarker>()
        .context("while connecting to power state control")?;
    let blobfs_verifier = connect_to_service::<BlobfsVerifierMarker>()
        .context("while connecting to blobfs verifier")?;

    let futures = FuturesUnordered::new();
    let (p_internal, p_external) = zx::EventPair::create().context("while creating EventPairs")?;

    // Keep a copy of the internal pair so that external consumers don't observe EVENTPAIR_CLOSED.
    let _p_internal_clone =
        p_internal.duplicate_handle(zx::Rights::NONE).context("while duplicating p_internal")?;

    let (unblocker, blocker) = oneshot::channel();

    // Handle putting boot metadata in happy state, rebooting on failure (if necessary), and
    // reporting health to the inspect health node.
    futures.push(
        async move {
            if let Err(e) = put_metadata_in_happy_state(
                &boot_manager,
                &p_internal,
                unblocker,
                &blobfs_verifier,
                verification_node_ref,
                &config,
            )
            .await
            {
                let msg = format!(
                    "Failed to put metadata in happy state. Rebooting given error {:#} and config {:?}",
                    anyhow!(e),
                    config
                );
                health_node_ref.set_unhealthy(&msg);
                fx_log_warn!("{}", msg);
                wait_and_reboot(&reboot_proxy, reboot_timer).await;
            } else {
                fx_log_info!("metadata is in happy state!");
                health_node_ref.set_ok();
            }
        }
        .boxed_local(),
    );

    // Handle ServiceFs and inspect
    let mut fs = ServiceFs::new_local();
    fs.take_and_serve_directory_handle().context("while serving directory handle")?;
    let () = inspector.serve(&mut fs).context("while serving inspect")?;

    // Handle FIDL.
    let fidl = Arc::new(FidlServer::new(p_external, blocker));
    futures.push(FidlServer::run(fidl, fs).boxed_local());

    let () = futures.collect::<()>().await;

    Ok(())
}
