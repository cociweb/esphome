#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/ring_buffer/ring_buffer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"

#include <atomic>
#include <memory>
#include <string>

namespace esphome::a2dp_sink {

/// @brief Internal event types posted from BT callbacks to the main loop.
enum class A2DPEvent : uint8_t {
  CONNECTED,
  DISCONNECTED,
  AUDIO_STARTED,
  AUDIO_STOPPED,
  AUDIO_CFG_UPDATED,
  PEER_NAME_UPDATED,
};

/// @brief Minimal event record posted onto the FreeRTOS queue.
struct A2DPEventRecord {
  A2DPEvent type;
  uint16_t sample_rate;
  uint8_t channels;
  char peer_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
};

/**
 * @brief ESPHome component that implements an A2DP Bluetooth audio sink.
 *
 * Manages the full BT Classic lifecycle (controller, Bluedroid, A2DP sink,
 * AVRCP target) via ESP-IDF native APIs.  Audio PCM data is pushed into a
 * ring buffer (optionally PSRAM-backed) from the BT data callback, and
 * consumed by an A2DPSinkMediaSource in a separate FreeRTOS task.
 *
 * Only supported on the original ESP32 (BR/EDR capable).
 */
class A2DPSink : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  // --- Configuration setters (called from generated code) ---

  void set_device_name(const char *name) { this->device_name_ = name; }
  void set_auto_start(bool auto_start) { this->auto_start_ = auto_start; }
  void set_ring_buffer_size(size_t size) { this->ring_buffer_size_ = size; }
  void set_use_psram(bool use_psram) { this->use_psram_ = use_psram; }
  void set_sample_rate(uint32_t rate) { this->configured_sample_rate_ = rate; }
  void set_pcm_drain_throttle_ms(uint32_t ms) { this->pcm_drain_throttle_ms_ = ms; }
  void set_output_delay_ms(uint32_t ms) { this->output_delay_ms_ = ms; }
  void set_pipeline_delay_ms(uint32_t ms) { this->pipeline_delay_ms_ = ms; }

  void set_software_coexistence(bool v) { this->software_coexistence_ = v; }
  void set_prefer_bt_while_streaming(bool v) { this->prefer_bt_while_streaming_ = v; }
  void set_prefer_bt_while_discoverable(bool v) { this->prefer_bt_while_discoverable_ = v; }
  void set_pause_wifi_sources_on_connect(bool v) { this->pause_wifi_sources_on_connect_ = v; }

  // --- Runtime control ---

  /// @brief Enable BT Classic stack and start A2DP sink (make discoverable).
  void enable();

  /// @brief Stop A2DP sink and deinit BT Classic stack.
  void disable();

  // --- State accessors ---

  bool is_enabled() const { return this->enabled_; }
  bool is_connected() const { return this->connected_; }
  bool is_audio_streaming() const { return this->audio_streaming_; }
  const std::string &get_peer_name() const { return this->peer_name_; }

  /// @brief Actual sample rate reported by the A2DP audio config event.
  uint32_t get_actual_sample_rate() const { return this->actual_sample_rate_.load(); }
  uint8_t get_actual_channels() const { return this->actual_channels_.load(); }

  /// @brief Return raw ring buffer pointer for use by the media source task.
  ring_buffer::RingBuffer *get_ring_buffer() { return this->ring_buffer_.get(); }

  uint32_t get_output_delay_ms() const { return this->output_delay_ms_; }
  uint32_t get_pcm_drain_throttle_ms() const { return this->pcm_drain_throttle_ms_; }

  // --- Callback registration ---

  template<typename F>
  void add_on_connection_callback(F &&callback) {
    this->connection_callback_.add(std::forward<F>(callback));
  }

  template<typename F>
  void add_on_peer_name_callback(F &&callback) {
    this->peer_name_callback_.add(std::forward<F>(callback));
  }

  template<typename F>
  void add_on_audio_streaming_callback(F &&callback) {
    this->audio_streaming_callback_.add(std::forward<F>(callback));
  }

 protected:
  // --- BT stack lifecycle ---
  bool init_bt_();
  void deinit_bt_();
  void set_coex_preference_(bool prefer_bt);

  // --- Static ESP-IDF callbacks (forward to global instance) ---
  static void s_a2d_callback_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  static void s_a2d_data_callback_(const uint8_t *data, uint32_t len);
  static void s_avrc_tg_callback_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  static void s_gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

  // --- Instance-level event handlers ---
  void handle_a2d_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  void handle_audio_data_(const uint8_t *data, uint32_t len);
  void handle_avrc_tg_event_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  void handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

  // --- Configuration ---
  const char *device_name_{"ESPHome"};
  bool auto_start_{false};
  size_t ring_buffer_size_{131072};
  bool use_psram_{false};
  uint32_t configured_sample_rate_{44100};
  uint32_t pcm_drain_throttle_ms_{500};
  uint32_t output_delay_ms_{200};
  uint32_t pipeline_delay_ms_{200};

  bool software_coexistence_{false};
  bool prefer_bt_while_streaming_{true};
  bool prefer_bt_while_discoverable_{false};
  bool pause_wifi_sources_on_connect_{false};

  // --- Runtime state ---
  bool enabled_{false};
  bool connected_{false};
  bool audio_streaming_{false};
  std::atomic<uint32_t> actual_sample_rate_{44100};
  std::atomic<uint8_t> actual_channels_{2};
  std::string peer_name_;

  // --- FreeRTOS event queue (BT callbacks → loop()) ---
  QueueHandle_t event_queue_{nullptr};
  static constexpr uint8_t EVENT_QUEUE_LEN = 8;

  // --- Ring buffer ---
  std::unique_ptr<ring_buffer::RingBuffer> ring_buffer_;

  // --- Callbacks ---
  LazyCallbackManager<void(bool)> connection_callback_;
  LazyCallbackManager<void(const std::string &)> peer_name_callback_;
  LazyCallbackManager<void(bool)> audio_streaming_callback_;
};

/// @brief Global singleton pointer required by ESP-IDF static callbacks.
extern A2DPSink *global_a2dp_sink;

// --- Automation actions ---

template<typename... Ts>
class A2DPSinkEnableAction : public Action<Ts...>, public Parented<A2DPSink> {
 public:
  void play(const Ts &...x) override { this->parent_->enable(); }
};

template<typename... Ts>
class A2DPSinkDisableAction : public Action<Ts...>, public Parented<A2DPSink> {
 public:
  void play(const Ts &...x) override { this->parent_->disable(); }
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK
