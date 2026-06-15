#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/components/ring_buffer/ring_buffer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <atomic>
#include <memory>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"

namespace esphome::a2dp {

/// @brief Internal event types posted from BT callbacks to the main loop.
enum class A2DPEvent : uint8_t {
  CONNECTED,
  DISCONNECTED,
  AUDIO_STARTED,
  AUDIO_STOPPED,
  AUDIO_CFG_UPDATED,
  PEER_NAME_UPDATED,
#ifdef USE_A2DP_AVRCP
  AVRCP_VOLUME_CHANGED,
  AVRCP_CT_CONNECTED,
  AVRCP_CT_DISCONNECTED,
  AVRCP_METADATA_UPDATED,
  AVRCP_TRACK_CHANGED,
#endif
};

/// @brief Minimal event record posted onto the FreeRTOS queue.
struct A2DPEventRecord {
  A2DPEvent type;
  uint16_t sample_rate;
  uint32_t bitrate;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint8_t channel_mode;
  uint8_t block_length;
  uint8_t subbands;
  uint8_t allocation_method;
  uint8_t min_bitpool;
  uint8_t max_bitpool;
  uint8_t volume;  ///< AVRCP_VOLUME_CHANGED: 0-127
  uint8_t metadata_attr;
  esp_bd_addr_t remote_bda;
  char metadata[128];
  char peer_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
};

/**
 * @brief Central A2DP hub component.
 *
 * Owns the BT Classic stack lifecycle (controller, Bluedroid, GAP),
 * the ring buffer for PCM audio, and the FreeRTOS event queue.
 * A2DPSink and A2DPAVRCP are Parented subcomponents that register
 * their callbacks on this hub.
 *
 * Only supported on the original ESP32 (BR/EDR capable).
 */
class A2DP : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::BLUETOOTH; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  // --- Configuration setters ---

  void set_device_name(const char *name) { this->device_name_ = name; }
  void set_ring_buffer_size(size_t size) { this->ring_buffer_size_ = size; }
  void set_use_psram(bool use_psram) { this->use_psram_ = use_psram; }
  void set_auto_start(bool auto_start) { this->auto_start_ = auto_start; }
  void set_auto_reconnect(bool auto_reconnect) { this->auto_reconnect_ = auto_reconnect; }
  void set_discoverable_duration_ms(uint32_t ms) { this->discoverable_duration_ms_ = ms; }
  void set_keep_discoverable_after_connect(bool keep) { this->keep_discoverable_after_connect_ = keep; }
  void set_pairing_pin(const char *pin) {
    strncpy(this->pairing_pin_, pin, sizeof(this->pairing_pin_) - 1);
    this->pairing_pin_[sizeof(this->pairing_pin_) - 1] = '\0';
    this->pairing_pin_len_ = strlen(this->pairing_pin_);
  }
  void set_preferred_bits_per_sample(uint8_t bits_per_sample) {
    this->preferred_bits_per_sample_ = bits_per_sample;
  }

#ifdef USE_SOFTWARE_COEXISTENCE
  void set_software_coexistence(bool v) { this->software_coexistence_ = v; }
  void set_prefer_bt_while_streaming(bool v) { this->prefer_bt_while_streaming_ = v; }
  void set_prefer_bt_while_discoverable(bool v) { this->prefer_bt_while_discoverable_ = v; }
  void set_pause_wifi_sources_on_connect(bool v) { this->pause_wifi_sources_on_connect_ = v; }
#endif

  // --- Runtime control ---

  void enable();
  void disable();
  void restart_discovery();
  void request_audio_suspend();

  // --- State accessors ---

  bool is_enabled() const { return this->enabled_; }
  bool is_connected() const { return this->connected_; }
  bool is_audio_streaming() const { return this->audio_streaming_; }
  bool is_discoverable() const { return this->discoverable_; }
  const char *get_peer_name() const { return this->peer_name_; }

  std::shared_ptr<ring_buffer::RingBuffer> get_ring_buffer() { return this->ring_buffer_; }
  void set_audio_output_enabled(bool enabled) {
    this->audio_output_enabled_.store(enabled, std::memory_order_relaxed);
  }
  void reset_audio_buffer() {
    if (this->ring_buffer_ != nullptr)
      this->ring_buffer_->reset();
  }

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
  void add_on_audio_state_callback(F &&callback) {
    this->audio_state_callback_.add(std::forward<F>(callback));
  }

  template<typename F>
  void add_on_audio_cfg_callback(F &&callback) {
    this->audio_cfg_callback_.add(std::forward<F>(callback));
  }

#ifdef USE_A2DP_AVRCP
  template<typename F>
  void add_on_avrcp_volume_callback(F &&callback) {
    this->avrcp_volume_callback_.add(std::forward<F>(callback));
  }
  template<typename F>
  void add_on_avrcp_ct_state_callback(F &&callback) {
    this->avrcp_ct_state_callback_.add(std::forward<F>(callback));
  }
  template<typename F>
  void add_on_avrcp_metadata_callback(F &&callback) {
    this->avrcp_metadata_callback_.add(std::forward<F>(callback));
  }
  template<typename F>
  void add_on_avrcp_track_change_callback(F &&callback) {
    this->avrcp_track_change_callback_.add(std::forward<F>(callback));
  }

  uint8_t get_avrcp_volume() const { return this->avrcp_volume_; }
  bool is_avrcp_ct_connected() const { return this->avrcp_ct_connected_; }

  void send_avrc_passthrough(uint8_t key_code, bool refresh_metadata = false) {
    if (!this->avrcp_ct_connected_) {
      ESP_LOGW("a2dp", "AVRCP CT not connected — passthrough ignored");
      return;
    }
    uint8_t tl = this->avrc_ct_tl_;
    this->avrc_ct_tl_ = (this->avrc_ct_tl_ + 2) % 15;
    esp_avrc_ct_send_passthrough_cmd(tl, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd((tl + 1) % 15, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
    if (refresh_metadata) {
      this->avrcp_track_change_callback_.call();
      this->metadata_refresh_at_ = millis() + 500;
    }
  }

  void request_avrcp_metadata() {
    if (!this->avrcp_ct_connected_)
      return;
    uint8_t tl = this->avrc_ct_tl_;
    this->avrc_ct_tl_ = (this->avrc_ct_tl_ + 1) % 15;
    esp_avrc_ct_send_metadata_cmd(tl, ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
  }

  void request_avrcp_track_change_notification() {
    if (!this->avrcp_ct_connected_)
      return;
    uint8_t tl = this->avrc_ct_tl_;
    this->avrc_ct_tl_ = (this->avrc_ct_tl_ + 1) % 15;
    esp_avrc_ct_send_register_notification_cmd(tl, ESP_AVRC_RN_TRACK_CHANGE, 0);
  }
#endif

 protected:
  // --- BT stack lifecycle ---
  bool init_bt_();
  void deinit_bt_();
  void start_discovery_();
  void stop_discovery_();
  void reconnect_to_last_peer_();
  void save_peer_(const esp_bd_addr_t remote_bda);
  void set_coex_preference_(bool prefer_bt);

  // --- Static ESP-IDF callbacks ---
  static void s_a2d_callback_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  static void s_a2d_data_callback_(const uint8_t *data, uint32_t len);
  static void s_gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
#ifdef USE_A2DP_AVRCP
  static void s_avrc_tg_callback_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  static void s_avrc_ct_callback_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
#endif

  // --- Instance-level handlers ---
  void handle_a2d_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
  void handle_audio_data_(const uint8_t *data, uint32_t len);
  void handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
#ifdef USE_A2DP_AVRCP
  void handle_avrc_tg_event_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);
  void handle_avrc_ct_event_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
#endif

  // --- Configuration ---
  const char *device_name_{"ESPHome"};
  size_t ring_buffer_size_{131072};
  bool use_psram_{false};
  bool auto_start_{false};
  bool auto_reconnect_{false};
  uint32_t discoverable_duration_ms_{0};
  bool keep_discoverable_after_connect_{false};
  char pairing_pin_[17]{};
  uint8_t pairing_pin_len_{0};
  uint8_t preferred_bits_per_sample_{16};
  ESPPreferenceObject peer_pref_;
  esp_bd_addr_t last_peer_bda_{};
  bool has_last_peer_{false};
  uint32_t reconnect_at_{0};
  uint8_t reconnect_attempts_{0};

#ifdef USE_SOFTWARE_COEXISTENCE
  bool software_coexistence_{false};
  bool prefer_bt_while_streaming_{true};
  bool prefer_bt_while_discoverable_{false};
  bool pause_wifi_sources_on_connect_{false};
#endif

  // --- Runtime state ---
  bool enabled_{false};
  bool connected_{false};
  bool audio_streaming_{false};
  std::atomic<bool> audio_output_enabled_{true};
  std::atomic<bool> audio_suspend_requested_{false};
  bool discoverable_{false};
  uint32_t discoverable_started_at_{0};
  char peer_name_[64]{};  ///< Last connected device name (truncated to 63 chars)
#ifdef USE_A2DP_AVRCP
  uint8_t avrcp_volume_{127};
  bool avrcp_ct_connected_{false};
  uint8_t avrc_ct_tl_{0};
  uint32_t metadata_refresh_at_{0};
#endif

  // --- FreeRTOS event queue ---
  QueueHandle_t event_queue_{nullptr};
  static constexpr uint8_t EVENT_QUEUE_LEN = 8;

  // --- Ring buffer ---
  std::shared_ptr<ring_buffer::RingBuffer> ring_buffer_;

  // --- Callbacks (consumed by subcomponents) ---
  LazyCallbackManager<void(bool)> connection_callback_;
  LazyCallbackManager<void(const char *)> peer_name_callback_;
  LazyCallbackManager<void(bool)> audio_state_callback_;
  LazyCallbackManager<void(uint16_t, uint8_t)> audio_cfg_callback_;  ///< sample_rate, channels
#ifdef USE_A2DP_AVRCP
  LazyCallbackManager<void(uint8_t)> avrcp_volume_callback_;
  LazyCallbackManager<void(bool)> avrcp_ct_state_callback_;
  LazyCallbackManager<void(uint8_t, const char *)> avrcp_metadata_callback_;
  LazyCallbackManager<void()> avrcp_track_change_callback_;
#endif
};

/// @brief Global singleton required by ESP-IDF static callbacks.
extern A2DP *global_a2dp;

// --- Automation actions ---

template<typename... Ts>
class A2DPEnableAction : public Action<Ts...>, public Parented<A2DP> {
 public:
  void play(const Ts &...x) override {
    ESP_LOGI("a2dp", "a2dp.enable action");
    this->parent_->enable();
  }
};

template<typename... Ts>
class A2DPDisableAction : public Action<Ts...>, public Parented<A2DP> {
 public:
  void play(const Ts &...x) override {
    ESP_LOGI("a2dp", "a2dp.disable action");
    this->parent_->disable();
  }
};

template<typename... Ts>
class A2DPRestartDiscoveryAction : public Action<Ts...>, public Parented<A2DP> {
 public:
  void play(const Ts &...x) override {
    ESP_LOGI("a2dp", "a2dp.restart_discovery action");
    this->parent_->restart_discovery();
  }
};

}  // namespace esphome::a2dp

#endif  // USE_ESP32 && USE_A2DP
