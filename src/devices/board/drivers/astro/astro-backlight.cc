// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/compiler.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/metadata.h>
#include <ddk/platform-defs.h>
#include <soc/aml-s905d2/s905d2-hw.h>

#include "astro.h"
#include "src/ui/backlight/drivers/ti-lp8556/ti-lp8556Metadata.h"

namespace astro {

constexpr pbus_mmio_t backlight_mmios[] = {
    {
        .base = S905D2_GPIO_A0_BASE,
        .length = S905D2_GPIO_AO_LENGTH,
    },
};

constexpr zx_bind_inst_t root_match[] = {
    BI_MATCH(),
};

constexpr zx_bind_inst_t i2c_match[] = {
    BI_ABORT_IF(NE, BIND_PROTOCOL, ZX_PROTOCOL_I2C),
    BI_ABORT_IF(NE, BIND_I2C_BUS_ID, ASTRO_I2C_3),
    BI_MATCH_IF(EQ, BIND_I2C_ADDRESS, I2C_BACKLIGHT_ADDR),
};

constexpr device_fragment_part_t i2c_fragment[] = {
    {countof(root_match), root_match},
    {countof(i2c_match), i2c_match},
};

constexpr device_fragment_t fragments[] = {
    {"i2c", countof(i2c_fragment), i2c_fragment},
};

constexpr double kMaxBrightnessInNits = 400.0;

TiLp8556Metadata kDeviceMetadata = {
    .panel_id = 0,
    .registers =
        {
            // Registers
            0x01, 0x85,  // Device Control
                         // EPROM
            0xa2, 0x30,  // CFG2
            0xa3, 0x32,  // CFG3
            0xa5, 0x54,  // CFG5
            0xa7, 0xf4,  // CFG7
            0xa9, 0x60,  // CFG9
            0xae, 0x09,  // CFGE
        },
    .register_count = 14,
};

const pbus_metadata_t backlight_metadata[] = {
    {
        .type = DEVICE_METADATA_BACKLIGHT_MAX_BRIGHTNESS_NITS,
        .data_buffer = reinterpret_cast<const uint8_t*>(&kMaxBrightnessInNits),
        .data_size = sizeof(kMaxBrightnessInNits),
    },
    {
        .type = DEVICE_METADATA_PRIVATE,
        .data_buffer = reinterpret_cast<const uint8_t*>(&kDeviceMetadata),
        .data_size = sizeof(kDeviceMetadata),
    },
};

constexpr pbus_dev_t backlight_dev = []() {
  pbus_dev_t dev = {};
  dev.name = "backlight";
  dev.vid = PDEV_VID_TI;
  dev.pid = PDEV_PID_TI_LP8556;
  dev.did = PDEV_DID_TI_BACKLIGHT;
  dev.metadata_list = backlight_metadata;
  dev.metadata_count = countof(backlight_metadata);
  dev.mmio_list = backlight_mmios;
  dev.mmio_count = countof(backlight_mmios);
  return dev;
}();

zx_status_t Astro::BacklightInit() {
  auto status = pbus_.CompositeDeviceAdd(&backlight_dev, reinterpret_cast<uint64_t>(fragments),
                                         countof(fragments), 1);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s CompositeDeviceAdd failed %d", __FUNCTION__, status);
  }
  return status;
}

}  // namespace astro
