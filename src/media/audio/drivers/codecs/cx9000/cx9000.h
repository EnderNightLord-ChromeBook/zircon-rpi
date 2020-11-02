// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_CODECS_CX9000_CX9000_H_
#define SRC_MEDIA_AUDIO_DRIVERS_CODECS_CX9000_CX9000_H_

#include <lib/device-protocol/i2c-channel.h>
#include <lib/zircon-internal/thread_annotations.h>
#include <threads.h>

#include <memory>

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddktl/device.h>
#include <ddktl/protocol/codec.h>
#include <ddktl/protocol/gpio.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>

#include "ddktl/suspend-txn.h"

namespace audio {

class Cx9000;
using DeviceType = ddk::Device<Cx9000, ddk::UnbindableNew, ddk::SuspendableNew>;

class Cx9000 : public DeviceType,  // Not final for unit tests.
               public ddk::CodecProtocol<Cx9000, ddk::base_protocol> {
 public:
  static zx_status_t Create(zx_device_t* parent);

  explicit Cx9000(zx_device_t* device, const ddk::I2cChannel& i2c,
                  const ddk::GpioProtocolClient& codec_reset)
      : DeviceType(device), i2c_(i2c), codec_reset_(codec_reset) {
    printf("1===%s\n", __PRETTY_FUNCTION__);
  }
  zx_status_t Bind();

  void DdkRelease() { delete this; }
  void DdkUnbindNew(ddk::UnbindTxn txn) {
    Shutdown();
    txn.Reply();
  }

  void DdkSuspendNew(ddk::SuspendTxn txn) {
    // TODO(fxb/42613): Implement proper power management based on the requested state.
    Shutdown();
    txn.Reply(ZX_OK, txn.requested_state());
  }

  // Codec protocol.
  void CodecReset(codec_reset_callback callback, void* cookie);
  void CodecGetInfo(codec_get_info_callback callback, void* cookie);
  void CodecIsBridgeable(codec_is_bridgeable_callback callback, void* cookie);
  void CodecSetBridgedMode(bool enable_bridged_mode, codec_set_bridged_mode_callback callback,
                           void* cookie);
  void CodecGetDaiFormats(codec_get_dai_formats_callback callback, void* cookie);
  void CodecSetDaiFormat(const dai_format_t* format, codec_set_dai_format_callback callback,
                         void* cookie);
  void CodecGetGainFormat(codec_get_gain_format_callback callback, void* cookie);
  void CodecGetGainState(codec_get_gain_state_callback callback, void* cookie);
  void CodecSetGainState(const gain_state_t* gain_state, codec_set_gain_state_callback callback,
                         void* cookie);
  void CodecGetPlugState(codec_get_plug_state_callback callback, void* cookie);

 protected:
  zx_status_t SoftwareResetAndInitialize();  // Protected for unit tests.
  zx_status_t HardwareReset();               // Protected for unit tests.

  std::atomic<bool> initialized_ = false;  // Protected for unit tests.

 private:
  static constexpr float kMaxGain = 0.0;
  static constexpr float kMinGain = -63.5;
  static constexpr float kGainStep = 0.5;

  zx_status_t WriteReg(uint16_t reg, uint8_t value) TA_REQ(lock_);
  zx_status_t WriteReg16(uint16_t reg, uint16_t value) TA_REQ(lock_);
  zx_status_t ReadReg(uint16_t reg, uint8_t* value) TA_REQ(lock_);
  void Shutdown();
  int Thread();

  float current_gain_ = 0;
  ddk::I2cChannel i2c_;
  ddk::GpioProtocolClient codec_reset_;
  thrd_t thread_;
  fbl::Mutex lock_;
};
}  // namespace audio

#endif  // SRC_MEDIA_AUDIO_DRIVERS_CODECS_CX9000_CX9000_H_
