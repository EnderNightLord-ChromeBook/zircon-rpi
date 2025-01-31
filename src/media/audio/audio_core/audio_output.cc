// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include "src/media/audio/audio_core/audio_output.h"

#include <lib/fit/defer.h>
#include <lib/trace/event.h>
#include <lib/zx/clock.h>

#include <limits>

#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/base_renderer.h"
#include "src/media/audio/audio_core/mixer/mixer.h"
#include "src/media/audio/audio_core/mixer/no_op.h"
#include "src/media/audio/audio_core/pin_executable_memory.h"
#include "src/media/audio/lib/logging/logging.h"

namespace media::audio {

// This MONOTONIC-based duration is the maximum interval between trim operations.
static constexpr zx::duration kMaxTrimPeriod = zx::msec(10);

// TODO(fxbug.dev/49345): We should not need driver to be set for all Audio Devices.
AudioOutput::AudioOutput(const std::string& name, ThreadingModel* threading_model,
                         DeviceRegistry* registry, LinkMatrix* link_matrix,
                         std::shared_ptr<AudioClockManager> clock_manager)
    : AudioDevice(Type::Output, name, threading_model, registry, link_matrix, clock_manager,
                  std::make_unique<AudioDriverV1>(this)),
      reporter_(Reporter::Singleton().CreateOutputDevice(name, mix_domain().name())) {
  SetNextSchedTimeMono(async::Now(mix_domain().dispatcher()));
}

AudioOutput::AudioOutput(const std::string& name, ThreadingModel* threading_model,
                         DeviceRegistry* registry, LinkMatrix* link_matrix,
                         std::shared_ptr<AudioClockManager> clock_manager,
                         std::unique_ptr<AudioDriver> driver)
    : AudioDevice(Type::Output, name, threading_model, registry, link_matrix, clock_manager,
                  std::move(driver)),
      reporter_(Reporter::Singleton().CreateOutputDevice(name, mix_domain().name())) {
  SetNextSchedTimeMono(async::Now(mix_domain().dispatcher()));
}

void AudioOutput::Process() {
  auto mono_now = async::Now(mix_domain().dispatcher());
  int64_t trace_wake_delta =
      next_sched_time_mono_.has_value() ? (mono_now - next_sched_time_mono_.value()).get() : 0;
  TRACE_DURATION("audio", "AudioOutput::Process", "wake delta", TA_INT64(trace_wake_delta));

  FX_DCHECK(pipeline_);

  // At this point, we should always know when our implementation would like to be called to do some
  // mixing work next. If we do not know, then we should have already shut down.
  //
  // If the next sched time has not arrived yet, don't attempt to mix anything. Just trim the queues
  // and move on.
  FX_DCHECK(next_sched_time_mono_);
  if (mono_now >= next_sched_time_mono_.value()) {
    // Clear the flag. If the implementation does not set it during the cycle by calling
    // SetNextSchedTimeMono, we consider it an error and shut down.
    ClearNextSchedTime();
    auto ref_now = reference_clock().ReferenceTimeFromMonotonicTime(mono_now);
    cpu_timer_.Start();

    uint32_t frames_remaining;
    do {
      float* payload = nullptr;
      auto mix_frames = StartMixJob(ref_now);
      // If we have frames to mix that are non-silent, we should do the mix now.
      if (mix_frames && !mix_frames->is_mute) {
        auto buf = pipeline_->ReadLock(Fixed(mix_frames->start), mix_frames->length);
        if (buf) {
          // We have a buffer so call FinishMixJob on this region and perform another MixJob if
          // we did not mix enough data. This can happen if our pipeline is unable to produce the
          // entire requested frame region in a single pass.
          FX_DCHECK(buf->start().Floor() == mix_frames->start);
          FX_DCHECK(buf->length().Floor() > 0);
          FX_DCHECK(pipeline_->format().sample_format() ==
                    fuchsia::media::AudioSampleFormat::FLOAT);
          payload = reinterpret_cast<float*>(buf->payload());

          // Reduce the frame range if we did not fill the entire requested frame region.
          int64_t buffer_length = buf->length().Floor();
          FX_CHECK(buffer_length >= 0);
          uint64_t valid_frames =
              std::min(mix_frames->length, static_cast<uint64_t>(buffer_length));
          frames_remaining = mix_frames->length - valid_frames;
          mix_frames->length = valid_frames;
        } else {
          // If the mix pipeline has no frames for this range, we treat this region as silence.
          // FinishMixJob will be responsible for filling this region of the ring with silence.
          mix_frames->is_mute = true;
          payload = nullptr;
          frames_remaining = 0;
        }
      } else {
        // If we did not |ReadLock| on this region of the pipeline, we should instead trim now to
        // ensure any client packets that otherwise would have been mixed are still released.
        pipeline_->Trim(Fixed::FromRaw(
            driver_ref_time_to_frac_safe_read_or_write_frame().Apply(ref_now.get())));
        frames_remaining = 0;
      }

      // If we have a mix job, we need to call |FinishMixJob| to commit these bytes to the hardware.
      if (mix_frames) {
        FinishMixJob(*mix_frames, payload);
      }
    } while (frames_remaining > 0);

    auto mono_end = async::Now(mix_domain().dispatcher());
    if (auto dt = mono_end - mono_now; dt > MixDeadline()) {
      cpu_timer_.Stop();
      TRACE_INSTANT("audio", "AudioOutput::MIX_UNDERFLOW", TRACE_SCOPE_THREAD);
      TRACE_ALERT("audio", "audiounderflow");
      FX_LOGS(ERROR("pipeline-underflow"))
          << "PIPELINE UNDERFLOW: Mixer ran for " << std::setprecision(4)
          << static_cast<double>(dt.to_nsecs()) / ZX_MSEC(1) << " ms, overran goal of "
          << static_cast<double>(MixDeadline().to_nsecs()) / ZX_MSEC(1) << " ms; thread spent "
          << cpu_timer_.cpu().get() << " ns on CPU, " << cpu_timer_.queue().get() << " ns queued";

      reporter().PipelineUnderflow(mono_now + MixDeadline(), mono_end);
    }
  }

  if (!next_sched_time_mono_) {
    FX_LOGS(ERROR) << "Output failed to schedule next service time. Shutting down!";
    ShutdownSelf();
    return;
  }

  // Figure out when we should wake up to do more work again. No matter how long our implementation
  // wants to wait, we need to make sure to wake up and periodically trim our input queues.
  auto max_sched_time_mono = mono_now + kMaxTrimPeriod;
  if (next_sched_time_mono_.value() > max_sched_time_mono) {
    SetNextSchedTimeMono(max_sched_time_mono);
  }
  zx_status_t status =
      mix_timer_.PostForTime(mix_domain().dispatcher(), next_sched_time_mono_.value());
  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to schedule mix";
    ShutdownSelf();
  }
}

fit::result<std::pair<std::shared_ptr<Mixer>, ExecutionDomain*>, zx_status_t>
AudioOutput::InitializeSourceLink(const AudioObject& source,
                                  std::shared_ptr<ReadableStream> source_stream) {
  TRACE_DURATION("audio", "AudioOutput::InitializeSourceLink");

  // If there's no source, use a Mixer that only trims, and no execution domain.
  if (!source_stream) {
    return fit::ok(std::make_pair(std::make_shared<audio::mixer::NoOp>(), nullptr));
  }

  auto usage = source.usage();
  FX_DCHECK(usage) << "Source has no assigned usage";
  if (!usage) {
    usage = {StreamUsage::WithRenderUsage(RenderUsage::MEDIA)};
  }

  float gain_db = Gain::kUnityGainDb;
  if (device_settings()) {
    auto [flags, cur_gain_state] = device_settings()->SnapshotGainState();
    gain_db = cur_gain_state.muted
                  ? fuchsia::media::audio::MUTED_GAIN_DB
                  : std::clamp(cur_gain_state.gain_db, Gain::kMinGainDb, Gain::kMaxGainDb);
  }

  // In rendering, we expect the source clock to originate from a client.
  // For now, "loop out" (direct device-to-device) routing is unsupported.
  FX_CHECK(source_stream->reference_clock().is_client_clock() ||
           source_stream->reference_clock() == reference_clock());

  auto mixer = pipeline_->AddInput(std::move(source_stream), *usage, gain_db);
  return fit::ok(std::make_pair(std::move(mixer), &mix_domain()));
}

void AudioOutput::CleanupSourceLink(const AudioObject& source,
                                    std::shared_ptr<ReadableStream> source_stream) {
  TRACE_DURATION("audio", "AudioOutput::CleanupSourceLink");
  if (source_stream) {
    pipeline_->RemoveInput(*source_stream);
  }
}

fit::result<std::shared_ptr<ReadableStream>, zx_status_t> AudioOutput::InitializeDestLink(
    const AudioObject& dest) {
  TRACE_DURATION("audio", "AudioOutput::InitializeDestLink");
  if (!pipeline_) {
    return fit::error(ZX_ERR_BAD_STATE);
  }
  return fit::ok(pipeline_->loopback());
}

std::unique_ptr<OutputPipeline> AudioOutput::CreateOutputPipeline(
    const PipelineConfig& config, const VolumeCurve& volume_curve, size_t max_block_size_frames,
    TimelineFunction device_reference_clock_to_fractional_frame, AudioClock& ref_clock) {
  auto pipeline =
      std::make_unique<OutputPipelineImpl>(config, volume_curve, max_block_size_frames,
                                           device_reference_clock_to_fractional_frame, ref_clock);
  pipeline->SetPresentationDelay(presentation_delay());
  return pipeline;
}

void AudioOutput::SetupMixTask(const DeviceConfig::OutputDeviceProfile& profile,
                               size_t max_block_size_frames,
                               TimelineFunction device_reference_clock_to_fractional_frame) {
  DeviceConfig updated_config = config();
  updated_config.SetOutputDeviceProfile(driver()->persistent_unique_id(), profile);
  set_config(updated_config);

  max_block_size_frames_ = max_block_size_frames;
  pipeline_ =
      CreateOutputPipeline(profile.pipeline_config(), profile.volume_curve(), max_block_size_frames,
                           device_reference_clock_to_fractional_frame, reference_clock());

  // In case the pipeline needs shared libraries, ensure those are paged in.
  PinExecutableMemory::Singleton().Pin();
}

void AudioOutput::Cleanup() {
  AudioDevice::Cleanup();
  mix_timer_.Cancel();
}

fit::promise<void, fuchsia::media::audio::UpdateEffectError> AudioOutput::UpdateEffect(
    const std::string& instance_name, const std::string& config) {
  fit::bridge<void, fuchsia::media::audio::UpdateEffectError> bridge;
  mix_domain().PostTask([this, self = shared_from_this(), instance_name, config,
                         completer = std::move(bridge.completer)]() mutable {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
    if (pipeline_ && !is_shutting_down()) {
      completer.complete_or_abandon(pipeline_->UpdateEffect(instance_name, config));
      return;
    }
    completer.complete_error(fuchsia::media::audio::UpdateEffectError::NOT_FOUND);
  });
  return bridge.consumer.promise();
}

fit::promise<void, zx_status_t> AudioOutput::UpdateDeviceProfile(
    const DeviceConfig::OutputDeviceProfile::Parameters& params) {
  fit::bridge<void, zx_status_t> bridge;
  mix_domain().PostTask([this, params, completer = std::move(bridge.completer)]() mutable {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &mix_domain());
    DeviceConfig device_config = config();
    auto current_profile = config().output_device_profile(driver()->persistent_unique_id());
    auto updated_profile = DeviceConfig::OutputDeviceProfile(
        params.eligible_for_loopback.value_or(current_profile.eligible_for_loopback()),
        params.supported_usages.value_or(current_profile.supported_usages()),
        params.volume_curve.value_or(current_profile.volume_curve()),
        params.independent_volume_control.value_or(current_profile.independent_volume_control()),
        params.pipeline_config.value_or(current_profile.pipeline_config()),
        params.driver_gain_db.value_or(current_profile.driver_gain_db()));
    device_config.SetOutputDeviceProfile(driver()->persistent_unique_id(), updated_profile);
    set_config(device_config);

    auto snapshot = pipeline_->ref_time_to_frac_presentation_frame();
    pipeline_ =
        CreateOutputPipeline(updated_profile.pipeline_config(), updated_profile.volume_curve(),
                             max_block_size_frames_, snapshot.timeline_function, reference_clock());
    FX_DCHECK(pipeline_);
    completer.complete_ok();
  });
  return bridge.consumer.promise();
}

void AudioOutput::SetGainInfo(const fuchsia::media::AudioGainInfo& info,
                              fuchsia::media::AudioGainValidFlags set_flags) {
  reporter_->SetGainInfo(info, set_flags);
  AudioDevice::SetGainInfo(info, set_flags);
}

}  // namespace media::audio
