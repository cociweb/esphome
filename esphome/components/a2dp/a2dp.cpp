#include "a2dp.h"

#if defined(USE_ESP32) && defined(USE_A2DP)

#include "esphome/core/log.h"
#include <algorithm>
#include <cstring>
#include <memory>

#if defined(CONFIG_BTDM_CONTROLLER_MODEM_SLEEP_EXT_WAKEUP) || defined(CONFIG_BTDM_COEX_SUPPORT)
#include "esp_coexist.h"
#define HAS_COEX_API
#endif

static const char *const TAG = "a2dp";

namespace esphome::a2dp {

A2DP *global_a2dp = nullptr;

struct SavedPeer {
  uint8_t valid;
  esp_bd_addr_t bda;
};

static constexpr uint32_t A2DP_PEER_PREF_HASH = 0xA2D90001UL;
static constexpr uint32_t RECONNECT_INITIAL_DELAY_MS = 1000;
static constexpr uint32_t RECONNECT_RETRY_DELAY_MS = 1000;
static constexpr uint8_t RECONNECT_MAX_ATTEMPTS = 5;

static void format_bda_(const esp_bd_addr_t bda, char *buf, size_t len) {
  snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static uint16_t sbc_sample_rate_(uint8_t samp_freq) {
  if (samp_freq <= 3) {
    static constexpr uint16_t rates[] = {16000, 32000, 44100, 48000};
    return rates[samp_freq];
  }
  if (samp_freq & 0x80)
    return 16000;
  if (samp_freq & 0x40)
    return 32000;
  if (samp_freq & 0x20)
    return 44100;
  if (samp_freq & 0x10)
    return 48000;
  return 0;
}

static uint8_t sbc_channels_(uint8_t ch_mode) {
  return (ch_mode & 0x08) ? 1 : 2;
}

static const char *sbc_channel_mode_name_(uint8_t ch_mode) {
  if (ch_mode & 0x08)
    return "mono";
  if (ch_mode & 0x04)
    return "dual_channel";
  if (ch_mode & 0x02)
    return "stereo";
  if (ch_mode & 0x01)
    return "joint_stereo";
  return "unknown";
}

static uint8_t sbc_block_length_(uint8_t block_len) {
  if (block_len & 0x08)
    return 4;
  if (block_len & 0x04)
    return 8;
  if (block_len & 0x02)
    return 12;
  if (block_len & 0x01)
    return 16;
  return 0;
}

static uint8_t sbc_subbands_(uint8_t num_subbands) {
  if (num_subbands & 0x02)
    return 4;
  if (num_subbands & 0x01)
    return 8;
  return 0;
}

static const char *sbc_allocation_name_(uint8_t alloc_mthd) {
  if (alloc_mthd & 0x02)
    return "snr";
  if (alloc_mthd & 0x01)
    return "loudness";
  return "unknown";
}

static uint32_t sbc_bitrate_(uint16_t sample_rate, uint8_t channels, uint8_t channel_mode, uint8_t blocks,
                             uint8_t subbands, uint8_t bitpool) {
  if (sample_rate == 0 || channels == 0 || blocks == 0 || subbands == 0 || bitpool == 0)
    return 0;
  uint16_t frame_bits = 32 + 4 * subbands * channels;
  if (channel_mode & 0x01) {
    frame_bits += subbands + blocks * bitpool;
  } else if ((channel_mode & 0x08) || (channel_mode & 0x02)) {
    frame_bits += blocks * bitpool;
  } else {
    frame_bits += blocks * bitpool * channels;
  }
  uint32_t frame_len = 4 + ((frame_bits + 7) / 8);
  return (8 * frame_len * sample_rate) / (subbands * blocks);
}

// ---------------------------------------------------------------------------
// ESP-IDF static callbacks
// ---------------------------------------------------------------------------

void A2DP::s_a2d_callback_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (global_a2dp != nullptr)
    global_a2dp->handle_a2d_event_(event, param);
}

void A2DP::s_a2d_data_callback_(const uint8_t *data, uint32_t len) {
  if (global_a2dp != nullptr)
    global_a2dp->handle_audio_data_(data, len);
}

void A2DP::s_gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (global_a2dp != nullptr)
    global_a2dp->handle_gap_event_(event, param);
}

#ifdef USE_A2DP_AVRCP
void A2DP::s_avrc_tg_callback_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  if (global_a2dp != nullptr)
    global_a2dp->handle_avrc_tg_event_(event, param);
}

void A2DP::s_avrc_ct_callback_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  if (global_a2dp != nullptr)
    global_a2dp->handle_avrc_ct_event_(event, param);
}
#endif

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void A2DP::setup() {
  ESP_LOGCONFIG(TAG, "Setting up A2DP hub...");

  global_a2dp = this;

  this->event_queue_ = xQueueCreate(EVENT_QUEUE_LEN, sizeof(A2DPEventRecord));
  if (this->event_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event queue");
    this->mark_failed();
    return;
  }

  auto pref = this->use_psram_ ? ring_buffer::RingBuffer::MemoryPreference::EXTERNAL_FIRST
                                : ring_buffer::RingBuffer::MemoryPreference::INTERNAL_FIRST;
  this->ring_buffer_ = ring_buffer::RingBuffer::create(this->ring_buffer_size_, pref);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate ring buffer (%u bytes)", (unsigned) this->ring_buffer_size_);
    this->mark_failed();
    return;
  }

  this->peer_pref_ = global_preferences->make_preference<SavedPeer>(A2DP_PEER_PREF_HASH, true);
  SavedPeer saved_peer{};
  if (this->peer_pref_.load(&saved_peer) && saved_peer.valid == 1) {
    memcpy(this->last_peer_bda_, saved_peer.bda, sizeof(this->last_peer_bda_));
    this->has_last_peer_ = true;
  }

  if (this->auto_start_) {
    this->enable();
  }
}

void A2DP::loop() {
  if (this->audio_suspend_requested_.exchange(false, std::memory_order_relaxed) && this->enabled_ &&
      this->connected_ && this->audio_streaming_) {
    esp_err_t ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    if (ret != ESP_OK)
      ESP_LOGW(TAG, "esp_a2d_media_ctrl(SUSPEND) failed: %s", esp_err_to_name(ret));
  }

  if (this->discoverable_ && this->discoverable_duration_ms_ > 0 &&
      (millis() - this->discoverable_started_at_) >= this->discoverable_duration_ms_) {
    this->stop_discovery_();
  }

  if (this->enabled_ && !this->connected_ && this->reconnect_at_ != 0 && millis() >= this->reconnect_at_) {
    this->reconnect_to_last_peer_();
  }

#ifdef USE_A2DP_AVRCP
  if (this->metadata_refresh_at_ != 0 && millis() >= this->metadata_refresh_at_) {
    this->metadata_refresh_at_ = 0;
    this->request_avrcp_metadata();
  }
#endif

  A2DPEventRecord ev;
  while (xQueueReceive(this->event_queue_, &ev, 0) == pdTRUE) {
    switch (ev.type) {
      case A2DPEvent::CONNECTED:
        if (!this->connected_) {
          this->connected_ = true;
          this->reconnect_at_ = 0;
          this->reconnect_attempts_ = 0;
          this->save_peer_(ev.remote_bda);
          if (!this->keep_discoverable_after_connect_)
            this->stop_discovery_();
          ESP_LOGI(TAG, "BT connected");
#ifdef USE_SOFTWARE_COEXISTENCE
          if (this->software_coexistence_ && !this->prefer_bt_while_discoverable_)
            this->set_coex_preference_(true);
#endif
          this->connection_callback_.call(true);
        }
        break;

      case A2DPEvent::DISCONNECTED:
        if (this->connected_) {
          this->connected_ = false;
          this->audio_streaming_ = false;
          this->audio_suspend_requested_.store(false, std::memory_order_relaxed);
          ESP_LOGI(TAG, "BT disconnected");
#ifdef USE_SOFTWARE_COEXISTENCE
          if (this->software_coexistence_)
            this->set_coex_preference_(false);
#endif
          this->connection_callback_.call(false);
          this->audio_state_callback_.call(false);
          this->start_discovery_();
        }
        break;

      case A2DPEvent::AUDIO_STARTED:
        if (!this->audio_streaming_) {
          this->audio_streaming_ = true;
          ESP_LOGI(TAG, "A2DP audio started");
#ifdef USE_SOFTWARE_COEXISTENCE
          if (this->software_coexistence_ && this->prefer_bt_while_streaming_)
            this->set_coex_preference_(true);
#endif
#ifdef USE_A2DP_AVRCP
          this->request_avrcp_metadata();
#endif
          this->audio_state_callback_.call(true);
        }
        break;

      case A2DPEvent::AUDIO_STOPPED:
        if (this->audio_streaming_) {
          this->audio_streaming_ = false;
          this->audio_suspend_requested_.store(false, std::memory_order_relaxed);
          ESP_LOGI(TAG, "A2DP audio stopped");
#ifdef USE_SOFTWARE_COEXISTENCE
          if (this->software_coexistence_ && this->prefer_bt_while_streaming_)
            this->set_coex_preference_(false);
#endif
          this->audio_state_callback_.call(false);
        }
        break;

      case A2DPEvent::AUDIO_CFG_UPDATED:
        ESP_LOGI(TAG,
                 "A2DP audio config: SBC, %u Hz, %u-bit, %u ch, %s, %u blocks, %u subbands, %s, bitpool %u-%u, "
                 "estimated max bitrate %u bps",
                 (unsigned) ev.sample_rate, (unsigned) ev.bits_per_sample, (unsigned) ev.channels,
                 sbc_channel_mode_name_(ev.channel_mode), (unsigned) ev.block_length, (unsigned) ev.subbands,
                 sbc_allocation_name_(ev.allocation_method), (unsigned) ev.min_bitpool, (unsigned) ev.max_bitpool,
                 (unsigned) ev.bitrate);
        this->audio_cfg_callback_.call(ev.sample_rate, ev.channels);
        break;

      case A2DPEvent::PEER_NAME_UPDATED:
        strncpy(this->peer_name_, ev.peer_name, sizeof(this->peer_name_) - 1);
        this->peer_name_[sizeof(this->peer_name_) - 1] = '\0';
        ESP_LOGI(TAG, "BT peer name: %s", this->peer_name_);
        this->peer_name_callback_.call(this->peer_name_);
        break;

#ifdef USE_A2DP_AVRCP
      case A2DPEvent::AVRCP_VOLUME_CHANGED:
        this->avrcp_volume_ = ev.volume;
        ESP_LOGD(TAG, "AVRCP volume: %u/127", ev.volume);
        this->avrcp_volume_callback_.call(ev.volume);
        break;

      case A2DPEvent::AVRCP_CT_CONNECTED:
        this->avrcp_ct_connected_ = true;
        ESP_LOGD(TAG, "AVRCP CT connected");
        this->avrcp_ct_state_callback_.call(true);
        this->request_avrcp_metadata();
        this->request_avrcp_track_change_notification();
        break;

      case A2DPEvent::AVRCP_CT_DISCONNECTED:
        this->avrcp_ct_connected_ = false;
        ESP_LOGD(TAG, "AVRCP CT disconnected");
        this->avrcp_ct_state_callback_.call(false);
        break;

      case A2DPEvent::AVRCP_METADATA_UPDATED:
        this->avrcp_metadata_callback_.call(ev.metadata_attr, ev.metadata);
        break;

      case A2DPEvent::AVRCP_TRACK_CHANGED:
        this->avrcp_track_change_callback_.call();
        this->metadata_refresh_at_ = millis() + 500;
        this->request_avrcp_track_change_notification();
        break;
#endif

      default:
        break;
    }
  }
}

void A2DP::dump_config() {
  ESP_LOGCONFIG(TAG, "A2DP Hub:");
  ESP_LOGCONFIG(TAG, "  Device Name:   %s", this->device_name_);
  ESP_LOGCONFIG(TAG, "  Ring Buffer:   %u bytes (%s)", (unsigned) this->ring_buffer_size_,
                this->use_psram_ ? "PSRAM" : "internal");
  ESP_LOGCONFIG(TAG, "  Auto Start:    %s", this->auto_start_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Reconnect:     %s", this->auto_reconnect_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Preferred PCM: %u-bit", (unsigned) this->preferred_bits_per_sample_);
#ifdef USE_SOFTWARE_COEXISTENCE
  if (this->software_coexistence_) {
    ESP_LOGCONFIG(TAG, "  Coexistence:   software");
  }
#endif
}

// ---------------------------------------------------------------------------
// Control: enable / disable
// ---------------------------------------------------------------------------

void A2DP::enable() {
  if (this->enabled_) {
    ESP_LOGD(TAG, "enable() called but already enabled");
    return;
  }
  if (!this->init_bt_()) {
    ESP_LOGE(TAG, "BT init failed");
    return;
  }
  this->enabled_ = true;
  ESP_LOGI(TAG, "A2DP hub enabled — waiting for connection");
  if (this->auto_reconnect_ && this->has_last_peer_) {
    this->reconnect_attempts_ = 0;
    this->reconnect_at_ = millis() + RECONNECT_INITIAL_DELAY_MS;
  }
  this->start_discovery_();
#ifdef USE_SOFTWARE_COEXISTENCE
  if (this->software_coexistence_ && this->prefer_bt_while_discoverable_)
    this->set_coex_preference_(true);
#endif
}

void A2DP::disable() {
  if (!this->enabled_) {
    ESP_LOGD(TAG, "disable() called but already disabled");
    return;
  }
  this->deinit_bt_();
  this->enabled_ = false;
  this->connected_ = false;
  this->audio_streaming_ = false;
  this->audio_suspend_requested_.store(false, std::memory_order_relaxed);
  this->reconnect_at_ = 0;
  this->reconnect_attempts_ = 0;
#ifdef USE_SOFTWARE_COEXISTENCE
  if (this->software_coexistence_)
    this->set_coex_preference_(false);
#endif
  if (this->ring_buffer_ != nullptr)
    this->ring_buffer_->reset();
  ESP_LOGI(TAG, "A2DP hub disabled");
}

void A2DP::restart_discovery() {
  if (!this->enabled_) {
    ESP_LOGW(TAG, "restart_discovery() called but BT not enabled — calling enable()");
    this->enable();
    return;
  }
  if (this->connected_) {
    ESP_LOGD(TAG, "restart_discovery() called but device is connected — ignoring");
    return;
  }
  this->start_discovery_();
}

void A2DP::request_audio_suspend() {
  this->audio_suspend_requested_.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// BT stack init / deinit
// ---------------------------------------------------------------------------

bool A2DP::init_bt_() {
  esp_err_t ret;

  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&cfg);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(ret));
      return false;
    }
  }

  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(ret));
      return false;
    }
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(ret));
      return false;
    }
  }

  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(ret));
      return false;
    }
  }

  ret = esp_bt_gap_register_callback(s_gap_callback_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_bt_gap_register_callback failed: %s", esp_err_to_name(ret));
    return false;
  }
  ret = esp_bt_gap_set_device_name(this->device_name_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_bt_gap_set_device_name failed: %s", esp_err_to_name(ret));
    return false;
  }

#ifdef USE_A2DP_AVRCP
  esp_avrc_ct_register_callback(s_avrc_ct_callback_);
  ret = esp_avrc_ct_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "esp_avrc_ct_init failed: %s (CT transport control unavailable)", esp_err_to_name(ret));
  }

  esp_avrc_tg_register_callback(s_avrc_tg_callback_);
  ret = esp_avrc_tg_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_avrc_tg_init failed: %s", esp_err_to_name(ret));
    return false;
  }
#endif

  esp_a2d_register_callback(s_a2d_callback_);

  ret = esp_a2d_sink_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_a2d_sink_init failed: %s", esp_err_to_name(ret));
    return false;
  }

  esp_a2d_sink_register_data_callback(s_a2d_data_callback_);

  return true;
}

void A2DP::deinit_bt_() {
  this->discoverable_ = false;
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

#ifdef USE_A2DP_AVRCP
  esp_avrc_ct_deinit();
  esp_avrc_tg_deinit();
  this->avrcp_ct_connected_ = false;
#endif
  esp_a2d_sink_deinit();

  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
}

// ---------------------------------------------------------------------------
// Discovery helpers
// ---------------------------------------------------------------------------

void A2DP::start_discovery_() {
  esp_err_t ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_bt_gap_set_scan_mode(discoverable) failed: %s", esp_err_to_name(ret));
    return;
  }
  this->discoverable_ = true;
  this->discoverable_started_at_ = millis();
  if (this->discoverable_duration_ms_ > 0) {
    ESP_LOGI(TAG, "BT discoverable for %u ms", this->discoverable_duration_ms_);
  } else {
    ESP_LOGI(TAG, "BT discoverable (indefinite)");
  }
}

void A2DP::stop_discovery_() {
  if (!this->discoverable_)
    return;
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  this->discoverable_ = false;
  ESP_LOGI(TAG, "BT discovery stopped");
}

void A2DP::reconnect_to_last_peer_() {
  this->reconnect_at_ = 0;
  this->reconnect_attempts_++;
  char bda[18];
  format_bda_(this->last_peer_bda_, bda, sizeof(bda));
  esp_err_t ret = esp_a2d_sink_connect(this->last_peer_bda_);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Reconnect to %s failed to start: %s", bda, esp_err_to_name(ret));
    if (ret == ESP_ERR_INVALID_STATE && this->reconnect_attempts_ < RECONNECT_MAX_ATTEMPTS) {
      this->reconnect_at_ = millis() + RECONNECT_RETRY_DELAY_MS;
    }
    return;
  }
  ESP_LOGI(TAG, "Reconnect to last A2DP source requested: %s", bda);
}

void A2DP::save_peer_(const esp_bd_addr_t remote_bda) {
  memcpy(this->last_peer_bda_, remote_bda, sizeof(this->last_peer_bda_));
  this->has_last_peer_ = true;
  SavedPeer saved_peer{};
  saved_peer.valid = 1;
  memcpy(saved_peer.bda, remote_bda, sizeof(saved_peer.bda));
  if (this->peer_pref_.save(&saved_peer)) {
    global_preferences->sync();
  }
}

// ---------------------------------------------------------------------------
// WiFi/BT coexistence
// ---------------------------------------------------------------------------

void A2DP::set_coex_preference_(bool prefer_bt) {
#ifdef HAS_COEX_API
  esp_coex_preference_t pref = prefer_bt ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_WIFI;
  esp_err_t ret = esp_coex_preference_set(pref);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "esp_coex_preference_set failed: %s", esp_err_to_name(ret));
  }
#endif
}

// ---------------------------------------------------------------------------
// A2DP callback handler
// ---------------------------------------------------------------------------

void A2DP::handle_a2d_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  A2DPEventRecord ev{};

  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
      auto state = param->conn_stat.state;
      if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        ev.type = A2DPEvent::CONNECTED;
        memcpy(ev.remote_bda, param->conn_stat.remote_bda, sizeof(ev.remote_bda));
        esp_bt_gap_read_remote_name(param->conn_stat.remote_bda);
        xQueueSend(this->event_queue_, &ev, 0);
      } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        ev.type = A2DPEvent::DISCONNECTED;
        xQueueSend(this->event_queue_, &ev, 0);
      }
      break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
      auto state = param->audio_stat.state;
      if (state == ESP_A2D_AUDIO_STATE_STARTED) {
        ev.type = A2DPEvent::AUDIO_STARTED;
      } else if (state == ESP_A2D_AUDIO_STATE_STOPPED || state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
        ev.type = A2DPEvent::AUDIO_STOPPED;
      } else {
        break;
      }
      xQueueSend(this->event_queue_, &ev, 0);
      break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
      if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
        auto &sbc = param->audio_cfg.mcc.cie.sbc_info;
        ev.sample_rate = sbc_sample_rate_(sbc.samp_freq);
        ev.channels = sbc_channels_(sbc.ch_mode);
        ev.bits_per_sample = 16;
        ev.channel_mode = sbc.ch_mode;
        ev.block_length = sbc_block_length_(sbc.block_len);
        ev.subbands = sbc_subbands_(sbc.num_subbands);
        ev.allocation_method = sbc.alloc_mthd;
        ev.min_bitpool = sbc.min_bitpool;
        ev.max_bitpool = sbc.max_bitpool;
        ev.bitrate = sbc_bitrate_(ev.sample_rate, ev.channels, ev.channel_mode, ev.block_length, ev.subbands,
                                  ev.max_bitpool);
        ev.type = A2DPEvent::AUDIO_CFG_UPDATED;
        xQueueSend(this->event_queue_, &ev, 0);
      }
      break;
    }

    default:
      break;
  }
}

void A2DP::handle_audio_data_(const uint8_t *data, uint32_t len) {
  if (this->audio_output_enabled_.load(std::memory_order_relaxed) && this->ring_buffer_ != nullptr)
    this->ring_buffer_->write(data, len);
}

void A2DP::handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT && param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
    A2DPEventRecord ev{};
    ev.type = A2DPEvent::PEER_NAME_UPDATED;
    strncpy(ev.peer_name, reinterpret_cast<const char *>(param->read_rmt_name.rmt_name),
            sizeof(ev.peer_name) - 1);
    ev.peer_name[sizeof(ev.peer_name) - 1] = '\0';
    xQueueSend(this->event_queue_, &ev, 0);
  }
}

#ifdef USE_A2DP_AVRCP
void A2DP::handle_avrc_tg_event_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  switch (event) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
      uint8_t vol = param->set_abs_vol.volume;
      esp_avrc_rn_param_t rn_param;
      rn_param.volume = vol;
      esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
      A2DPEventRecord ev{};
      ev.type = A2DPEvent::AVRCP_VOLUME_CHANGED;
      ev.volume = vol;
      xQueueSend(this->event_queue_, &ev, 0);
      break;
    }
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
      if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
        esp_avrc_rn_param_t rn_param;
        rn_param.volume = this->avrcp_volume_;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
      }
      break;
    }
    default:
      break;
  }
}

void A2DP::handle_avrc_ct_event_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
      A2DPEventRecord ev{};
      ev.type = param->conn_stat.connected ? A2DPEvent::AVRCP_CT_CONNECTED : A2DPEvent::AVRCP_CT_DISCONNECTED;
      xQueueSend(this->event_queue_, &ev, 0);
      break;
    }
    case ESP_AVRC_CT_METADATA_RSP_EVT: {
      A2DPEventRecord ev{};
      ev.type = A2DPEvent::AVRCP_METADATA_UPDATED;
      ev.metadata_attr = param->meta_rsp.attr_id;
      size_t len = 0;
      if (param->meta_rsp.attr_text != nullptr && param->meta_rsp.attr_length > 0) {
        len = std::min<size_t>(param->meta_rsp.attr_length, sizeof(ev.metadata) - 1);
        memcpy(ev.metadata, param->meta_rsp.attr_text, len);
      }
      ev.metadata[len] = '\0';
      xQueueSend(this->event_queue_, &ev, 0);
      break;
    }
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
      if (param->change_ntf.event_id == ESP_AVRC_RN_TRACK_CHANGE) {
        A2DPEventRecord ev{};
        ev.type = A2DPEvent::AVRCP_TRACK_CHANGED;
        xQueueSend(this->event_queue_, &ev, 0);
      }
      break;
    }
    default:
      break;
  }
}
#endif

}  // namespace esphome::a2dp

#endif  // USE_ESP32 && USE_A2DP
