// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::base::SettingType,
    crate::call_async,
    crate::handler::base::Request,
    crate::handler::setting_handler::{
        controller, ClientProxy, ControllerError, SettingHandlerResult,
    },
    crate::service_context::ServiceContextHandle,
    async_trait::async_trait,
    fidl_fuchsia_hardware_power_statecontrol::RebootReason,
    fuchsia_syslog::fx_log_err,
};

async fn reboot(service_context_handle: &ServiceContextHandle) -> Result<(), ControllerError> {
    let hardware_power_statecontrol_admin = service_context_handle
        .lock()
        .await
        .connect::<fidl_fuchsia_hardware_power_statecontrol::AdminMarker>()
        .await
        .or_else(|_| {
            Err(ControllerError::ExternalFailure(
                SettingType::Power,
                "hardware_power_statecontrol_manager".into(),
                "connect".into(),
            ))
        })?;

    let build_err = || {
        ControllerError::ExternalFailure(
            SettingType::Power,
            "hardware_power_statecontrol_manager".into(),
            "reboot".into(),
        )
    };

    call_async!(hardware_power_statecontrol_admin => reboot(RebootReason::UserRequest))
        .await
        .map_err(|_| build_err())
        .and_then(|r| {
            r.map_err(|zx_status| {
                fx_log_err!("Failed to reboot device: {}", zx_status);
                build_err()
            })
        })
}

pub struct PowerController {
    service_context: ServiceContextHandle,
}

#[async_trait]
impl controller::Create for PowerController {
    async fn create(client: ClientProxy) -> Result<Self, ControllerError> {
        let service_context = client.get_service_context().await;
        Ok(Self { service_context })
    }
}

#[async_trait]
impl controller::Handle for PowerController {
    async fn handle(&self, request: Request) -> Option<SettingHandlerResult> {
        match request {
            Request::Reboot => Some(reboot(&self.service_context).await.map(|_| None)),
            _ => None,
        }
    }
}
