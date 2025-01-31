// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_INTEL_HDA_CONTROLLER_INTEL_DSP_STREAM_H_
#define SRC_MEDIA_AUDIO_DRIVERS_INTEL_HDA_CONTROLLER_INTEL_DSP_STREAM_H_

#include <ddk/device.h>
#include <intel-hda/codec-utils/stream-base.h>

#include "debug-logging.h"
#include "intel-dsp-topology.h"

namespace audio {
namespace intel_hda {

class IntelDspStream : public codecs::IntelHDAStreamBase {
 public:
  IntelDspStream(uint32_t id, bool is_input, const DspPipeline& pipeline, fbl::String name,
                 const audio_stream_unique_id_t* unique_id = nullptr);

  // Overloaded
  zx_status_t ProcessSetStreamFmt(const ihda_proto::SetStreamFmtResp& resp,
                                  zx::channel&& ring_buffer_channel)
      __TA_EXCLUDES(obj_lock()) override;

  const char* log_prefix() const { return log_prefix_; }
  void RingBufferChannelSignalled(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                  zx_status_t status, const zx_packet_signal_t* signal);
  void ClientRingBufferChannelSignalled(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                        zx_status_t status, const zx_packet_signal_t* signal);

 protected:
  virtual ~IntelDspStream() {}

  zx_status_t OnActivateLocked() __TA_REQUIRES(obj_lock()) final;
  void OnDeactivateLocked() __TA_REQUIRES(obj_lock()) final;
  void OnChannelDeactivateLocked(const Channel& channel) __TA_REQUIRES(obj_lock()) final;
  zx_status_t OnDMAAssignedLocked() __TA_REQUIRES(obj_lock()) final;
  zx_status_t OnSolicitedResponseLocked(const CodecResponse& resp) __TA_REQUIRES(obj_lock()) final;
  zx_status_t OnUnsolicitedResponseLocked(const CodecResponse& resp)
      __TA_REQUIRES(obj_lock()) final;
  zx_status_t BeginChangeStreamFormatLocked(const audio_proto::StreamSetFmtReq& fmt)
      __TA_REQUIRES(obj_lock()) final;
  zx_status_t FinishChangeStreamFormatLocked(uint16_t encoded_fmt) __TA_REQUIRES(obj_lock()) final;
  void OnGetGainLocked(audio_proto::GetGainResp* out_resp) __TA_REQUIRES(obj_lock()) final;
  void OnSetGainLocked(const audio_proto::SetGainReq& req, audio_proto::SetGainResp* out_resp)
      __TA_REQUIRES(obj_lock()) final;
  void OnPlugDetectLocked(Channel* response_channel, const audio_proto::PlugDetectReq& req,
                          audio_proto::PlugDetectResp* out_resp) __TA_REQUIRES(obj_lock()) final;
  void OnGetStringLocked(const audio_proto::GetStringReq& req, audio_proto::GetStringResp* out_resp)
      __TA_REQUIRES(obj_lock()) final;

 private:
  friend class fbl::RefPtr<IntelDspStream>;

  zx_status_t ProcessRbRequestLocked(Channel* channel) __TA_REQUIRES(obj_lock());
  void ProcessRbDeactivate();

  zx_status_t ProcessClientRbRequestLocked(Channel* channel) __TA_REQUIRES(obj_lock());
  void ProcessClientRbDeactivate();

  // Device name, exposed to the user.
  const fbl::String name_;

  // Log prefix storage
  char log_prefix_[LOG_PREFIX_STORAGE] = {0};

  const DspPipeline pipeline_;

  fbl::RefPtr<Channel> rb_channel_ __TA_GUARDED(obj_lock());
  fbl::RefPtr<Channel> client_rb_channel_ __TA_GUARDED(obj_lock());
};

}  // namespace intel_hda
}  // namespace audio

#endif  // SRC_MEDIA_AUDIO_DRIVERS_INTEL_HDA_CONTROLLER_INTEL_DSP_STREAM_H_
