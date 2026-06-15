#include "a2dp_sink.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK)

#include "esphome/core/log.h"

static const char *const TAG = "a2dp_sink";

namespace esphome::a2dp_sink {

void A2DPSink::setup() {
  this->parent_->add_on_audio_state_callback([this](bool streaming) {
    this->audio_streaming_ = streaming;
    this->audio_streaming_callback_.call(streaming);
  });

  this->parent_->add_on_audio_cfg_callback([this](uint16_t sample_rate, uint8_t channels) {
    this->actual_sample_rate_ = sample_rate;
    this->actual_channels_ = channels;
    ESP_LOGD(TAG, "Audio config: %u Hz, %u ch", (unsigned) sample_rate, (unsigned) channels);
  });
}

void A2DPSink::dump_config() {
  ESP_LOGCONFIG(TAG, "A2DP Sink:");
  ESP_LOGCONFIG(TAG, "  Sample Rate:        %u Hz", (unsigned) this->configured_sample_rate_);
  ESP_LOGCONFIG(TAG, "  Output Delay:       %u ms", (unsigned) this->output_delay_ms_);
  ESP_LOGCONFIG(TAG, "  PCM Drain Throttle: %u ms", (unsigned) this->pcm_drain_throttle_ms_);
}

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK
