#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/a2dp/a2dp.h"

#include <atomic>

namespace esphome::a2dp_sink {

/**
 * @brief A2DP Sink subcomponent — Parented<a2dp::A2DP>.
 *
 * Registers callbacks on the hub to track audio streaming state and
 * exposes accessors used by the media source task and sensor subcomponents.
 * BT stack lifecycle, ring buffer, and GAP are owned by the parent A2DP hub.
 */
class A2DPSink : public Component, public Parented<a2dp::A2DP> {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
  void setup() override;
  void dump_config() override;

  // --- Configuration setters ---

  void set_sample_rate(uint32_t rate) { this->configured_sample_rate_ = rate; }
  void set_pcm_drain_throttle_ms(uint32_t ms) { this->pcm_drain_throttle_ms_ = ms; }
  void set_output_delay_ms(uint32_t ms) { this->output_delay_ms_ = ms; }
  void set_pipeline_delay_ms(uint32_t ms) { this->pipeline_delay_ms_ = ms; }

  // --- State accessors ---

  bool is_audio_streaming() const { return this->audio_streaming_; }

  /// @brief Actual sample rate reported by the A2DP audio config event (thread-safe).
  uint32_t get_actual_sample_rate() const { return this->actual_sample_rate_.load(); }
  uint8_t get_actual_channels() const { return this->actual_channels_.load(); }

  uint32_t get_output_delay_ms() const { return this->output_delay_ms_; }
  uint32_t get_pcm_drain_throttle_ms() const { return this->pcm_drain_throttle_ms_; }

  /// @brief Ring buffer pointer (delegates to hub).
  ring_buffer::RingBuffer *get_ring_buffer() { return this->parent_->get_ring_buffer(); }

  // --- Callback registration ---

  template<typename F>
  void add_on_audio_streaming_callback(F &&callback) {
    this->audio_streaming_callback_.add(std::forward<F>(callback));
  }

 protected:
  // --- Configuration ---
  uint32_t configured_sample_rate_{44100};
  uint32_t pcm_drain_throttle_ms_{500};
  uint32_t output_delay_ms_{200};
  uint32_t pipeline_delay_ms_{200};

  // --- Runtime state ---
  bool audio_streaming_{false};
  std::atomic<uint32_t> actual_sample_rate_{44100};
  std::atomic<uint8_t> actual_channels_{2};

  // --- Callbacks ---
  LazyCallbackManager<void(bool)> audio_streaming_callback_;
};

// --- Automation actions (delegate to hub) ---

template<typename... Ts>
class A2DPSinkEnableAction : public Action<Ts...>, public Parented<A2DPSink> {
 public:
  void play(const Ts &...x) override { this->parent_->get_parent()->enable(); }
};

template<typename... Ts>
class A2DPSinkDisableAction : public Action<Ts...>, public Parented<A2DPSink> {
 public:
  void play(const Ts &...x) override { this->parent_->get_parent()->disable(); }
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK
