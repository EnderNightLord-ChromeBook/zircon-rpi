// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::base::SettingInfo;
use crate::handler::base::Request;
use crate::handler::device_storage::DeviceStorageCompatible;
use crate::handler::setting_handler::persist::{
    controller as data_controller, write, ClientProxy, WriteResult,
};
use crate::handler::setting_handler::{controller, ControllerError, SettingHandlerResult};
use crate::night_mode::types::NightModeInfo;
use async_trait::async_trait;

impl DeviceStorageCompatible for NightModeInfo {
    const KEY: &'static str = "night_mode_info";

    fn default_value() -> Self {
        NightModeInfo { night_mode_enabled: None }
    }
}

impl Into<SettingInfo> for NightModeInfo {
    fn into(self) -> SettingInfo {
        SettingInfo::NightMode(self)
    }
}

pub struct NightModeController {
    client: ClientProxy<NightModeInfo>,
}

#[async_trait]
impl data_controller::Create<NightModeInfo> for NightModeController {
    /// Creates the controller
    async fn create(client: ClientProxy<NightModeInfo>) -> Result<Self, ControllerError> {
        Ok(NightModeController { client: client })
    }
}

#[async_trait]
impl controller::Handle for NightModeController {
    async fn handle(&self, request: Request) -> Option<SettingHandlerResult> {
        match request {
            Request::SetNightModeInfo(night_mode_info) => {
                let mut current = self.client.read().await;

                // Save the value locally.
                current.night_mode_enabled = night_mode_info.night_mode_enabled;
                Some(write(&self.client, current, false).await.into_handler_result())
            }
            Request::Get => Some(Ok(Some(SettingInfo::NightMode(self.client.read().await)))),
            _ => None,
        }
    }
}
