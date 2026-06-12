#include "a2dp_sink.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK)

#include "esphome/core/log.h"

#if defined(CONFIG_BTDM_CONTROLLER_MODEM_SLEEP_EXT_WAKEUP) || defined(CONFIG_BTDM_COEX_SUPPORT)
#include "esp_coexist.h"
#define HAS_COEX_API
#endif

static const char *const TAG = "a2dp_sink";

namespace esphome::a2dp_sink {

A2DPSink *global_a2dp_sink = nullptr;

// ---------------------------------------------------------------------------
// ESP-IDF static callbacks — forward to the global instance
// ---------------------------------------------------------------------------

void A2DPSink::s_a2d_callback_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  if (global_a2dp_sink != nullptr)
    global_a2dp_sink->handle_a2d_event_(event, param);
}

void A2DPSink::s_a2d_data_callback_(const uint8_t *data, uint32_t len) {
  if (global_a2dp_sink != nullptr)
    global_a2dp_sink->handle_audio_data_(data, len);
}

void A2DPSink::s_avrc_tg_callback_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  if (global_a2dp_sink != nullptr)
    global_a2dp_sink->handle_avrc_tg_event_(event, param);
}

void A2DPSink::s_avrc_ct_callback_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  if (global_a2dp_sink != nullptr)
    global_a2dp_sink->handle_avrc_ct_event_(event, param);
}

void A2DPSink::s_gap_callback_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (global_a2dp_sink != nullptr)
    global_a2dp_sink->handle_gap_event_(event, param);
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void A2DPSink::setup() {
  ESP_LOGCONFIG(TAG, "Setting up A2DP Sink...");

  global_a2dp_sink = this;

  this->event_queue_ = xQueueCreate(EVENT_QUEUE_LEN, sizeof(A2DPEventRecord));
  if (this->event_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event queue");
    this->mark_failed();
    return;
  }

  // Pre-allocate ring buffer at setup time so it is not allocated on-demand.
  auto pref = this->use_psram_ ? ring_buffer::RingBuffer::MemoryPreference::EXTERNAL_FIRST
                                : ring_buffer::RingBuffer::MemoryPreference::INTERNAL_FIRST;
  this->ring_buffer_ = ring_buffer::RingBuffer::create(this->ring_buffer_size_, pref);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate ring buffer (%u bytes)", (unsigned) this->ring_buffer_size_);
    this->mark_failed();
    return;
  }

  if (this->auto_start_) {
    this->enable();
  }
}

void A2DPSink::loop() {
  if (this->discoverable_ && this->discoverable_duration_ms_ > 0 &&
      (millis() - this->discoverable_started_at_) >= this->discoverable_duration_ms_) {
    this->stop_discovery_();
  }

  A2DPEventRecord ev;
  while (xQueueReceive(this->event_queue_, &ev, 0) == pdTRUE) {
    switch (ev.type) {
      case A2DPEvent::CONNECTED:
        if (!this->connected_) {
          this->connected_ = true;
          this->stop_discovery_();
          ESP_LOGI(TAG, "BT connected");
          if (this->software_coexistence_ && !this->prefer_bt_while_discoverable_)
            this->set_coex_preference_(true);
          this->connection_callback_.call(true);
        }
        break;

      case A2DPEvent::DISCONNECTED:
        if (this->connected_) {
          this->connected_ = false;
          this->audio_streaming_ = false;
          ESP_LOGI(TAG, "BT disconnected");
          if (this->software_coexistence_)
            this->set_coex_preference_(false);
          this->connection_callback_.call(false);
          this->audio_streaming_callback_.call(false);
          this->start_discovery_();
        }
        break;

      case A2DPEvent::AUDIO_STARTED:
        if (!this->audio_streaming_) {
          this->audio_streaming_ = true;
          ESP_LOGI(TAG, "A2DP audio started (%u Hz, %u ch)", (unsigned) this->actual_sample_rate_,
                   (unsigned) this->actual_channels_);
          if (this->software_coexistence_ && this->prefer_bt_while_streaming_)
            this->set_coex_preference_(true);
          this->audio_streaming_callback_.call(true);
        }
        break;

      case A2DPEvent::AUDIO_STOPPED:
        if (this->audio_streaming_) {
          this->audio_streaming_ = false;
          ESP_LOGI(TAG, "A2DP audio stopped");
          if (this->software_coexistence_ && this->prefer_bt_while_streaming_)
            this->set_coex_preference_(false);
          this->audio_streaming_callback_.call(false);
        }
        break;

      case A2DPEvent::AUDIO_CFG_UPDATED:
        this->actual_sample_rate_ = ev.sample_rate;
        this->actual_channels_ = ev.channels;
        ESP_LOGI(TAG, "A2DP audio config: %u Hz, %u ch", (unsigned) ev.sample_rate, (unsigned) ev.channels);
        break;

      case A2DPEvent::PEER_NAME_UPDATED:
        this->peer_name_ = ev.peer_name;
        ESP_LOGI(TAG, "BT peer name: %s", ev.peer_name);
        this->peer_name_callback_.call(this->peer_name_);
        break;

      case A2DPEvent::AVRCP_VOLUME_CHANGED:
        this->avrcp_volume_ = ev.volume;
        ESP_LOGD(TAG, "AVRCP volume: %u/127 (%.0f%%)", ev.volume, ev.volume * 100.0f / 127.0f);
        this->avrcp_volume_callback_.call(ev.volume);
        break;

      case A2DPEvent::AVRCP_CT_CONNECTED:
        this->avrcp_ct_connected_ = true;
        ESP_LOGD(TAG, "AVRCP CT connected");
        break;

      case A2DPEvent::AVRCP_CT_DISCONNECTED:
        this->avrcp_ct_connected_ = false;
        ESP_LOGD(TAG, "AVRCP CT disconnected");
        break;

      default:
        break;
    }
  }
}

void A2DPSink::dump_config() {
  ESP_LOGCONFIG(TAG, "A2DP Sink:");
  ESP_LOGCONFIG(TAG, "  Device Name:        %s", this->device_name_);
  ESP_LOGCONFIG(TAG, "  Ring Buffer:        %u bytes (%s)", (unsigned) this->ring_buffer_size_,
                this->use_psram_ ? "PSRAM" : "internal");
  ESP_LOGCONFIG(TAG, "  Auto Start:         %s", this->auto_start_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Sample Rate:        %u Hz", (unsigned) this->configured_sample_rate_);
  ESP_LOGCONFIG(TAG, "  Output Delay:       %u ms", (unsigned) this->output_delay_ms_);
  ESP_LOGCONFIG(TAG, "  PCM Drain Throttle: %u ms", (unsigned) this->pcm_drain_throttle_ms_);
  if (this->software_coexistence_) {
    ESP_LOGCONFIG(TAG, "  Coexistence:        software");
  }
}

// ---------------------------------------------------------------------------
// Control: enable / disable
// ---------------------------------------------------------------------------

void A2DPSink::enable() {
  if (this->enabled_) {
    ESP_LOGD(TAG, "enable() called but already enabled");
    return;
  }
  if (!this->init_bt_()) {
    ESP_LOGE(TAG, "BT init failed");
    return;
  }
  this->enabled_ = true;
  ESP_LOGI(TAG, "A2DP Sink enabled — waiting for connection");
  if (this->software_coexistence_ && this->prefer_bt_while_discoverable_)
    this->set_coex_preference_(true);
}

void A2DPSink::disable() {
  if (!this->enabled_) {
    ESP_LOGD(TAG, "disable() called but already disabled");
    return;
  }
  this->deinit_bt_();
  this->enabled_ = false;
  this->connected_ = false;
  this->audio_streaming_ = false;
  if (this->software_coexistence_)
    this->set_coex_preference_(false);
  this->ring_buffer_->reset();
  ESP_LOGI(TAG, "A2DP Sink disabled");
}

// ---------------------------------------------------------------------------
// BT stack init / deinit
// ---------------------------------------------------------------------------

bool A2DPSink::init_bt_() {
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

  esp_bt_gap_register_callback(s_gap_callback_);
  esp_bt_gap_set_device_name(this->device_name_);

  esp_a2d_register_callback(s_a2d_callback_);
  esp_a2d_sink_register_data_callback(s_a2d_data_callback_);

  ret = esp_a2d_sink_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_a2d_sink_init failed: %s", esp_err_to_name(ret));
    return false;
  }

  esp_avrc_tg_register_callback(s_avrc_tg_callback_);
  ret = esp_avrc_tg_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_avrc_tg_init failed: %s", esp_err_to_name(ret));
    return false;
  }

  esp_avrc_ct_register_callback(s_avrc_ct_callback_);
  ret = esp_avrc_ct_init();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "esp_avrc_ct_init failed: %s (CT transport control unavailable)", esp_err_to_name(ret));
  }

  this->start_discovery_();

  return true;
}

void A2DPSink::deinit_bt_() {
  this->discoverable_ = false;
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

  esp_avrc_ct_deinit();
  esp_avrc_tg_deinit();
  esp_a2d_sink_deinit();
  this->avrcp_ct_connected_ = false;

  esp_bluedroid_disable();
  esp_bluedroid_deinit();

  esp_bt_controller_disable();
  esp_bt_controller_deinit();
}

// ---------------------------------------------------------------------------
// Discovery helpers
// ---------------------------------------------------------------------------

void A2DPSink::start_discovery_() {
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  this->discoverable_ = true;
  this->discoverable_started_at_ = millis();
  if (this->discoverable_duration_ms_ > 0) {
    ESP_LOGI(TAG, "BT discoverable for %u ms", this->discoverable_duration_ms_);
  } else {
    ESP_LOGI(TAG, "BT discoverable (indefinite)");
  }
}

void A2DPSink::stop_discovery_() {
  if (!this->discoverable_)
    return;
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  this->discoverable_ = false;
  ESP_LOGI(TAG, "BT discovery stopped");
}

// ---------------------------------------------------------------------------
// WiFi/BT coexistence preference
// ---------------------------------------------------------------------------

void A2DPSink::set_coex_preference_(bool prefer_bt) {
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

void A2DPSink::handle_a2d_event_(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
  A2DPEventRecord ev{};

  switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
      auto state = param->conn_stat.state;
      if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        ev.type = A2DPEvent::CONNECTED;
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
      } else if (state == ESP_A2D_AUDIO_STATE_STOPPED ||
                 state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
        ev.type = A2DPEvent::AUDIO_STOPPED;
      } else {
        break;
      }
      xQueueSend(this->event_queue_, &ev, 0);
      break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
      if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
        auto &sbc = param->audio_cfg.mcc.cie.sbc;
        // Decode SBC sample frequency field (bits 6:4 of octet 0)
        static const uint16_t sbc_rates[] = {16000, 32000, 44100, 48000};
        uint8_t freq_idx = (sbc.samp_freq >> 4) & 0x0F;
        // Find the highest-bit set (rates ordered MSB first in the spec)
        for (int i = 0; i < 4; i++) {
          if (freq_idx & (0x08 >> i)) {
            ev.sample_rate = sbc_rates[i];
            break;
          }
        }
        if (ev.sample_rate == 0)
          ev.sample_rate = 44100;
        ev.channels = (sbc.ch_mode == 0) ? 1 : 2;  // 0 = MONO
        ev.type = A2DPEvent::AUDIO_CFG_UPDATED;
        // actual_sample_rate_ / actual_channels_ are updated atomically in loop()
        // after the event is dequeued, so the reader task always sees a consistent value.
        xQueueSend(this->event_queue_, &ev, 0);
      }
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Audio data callback — called from BT task at high frequency
// ---------------------------------------------------------------------------

void A2DPSink::handle_audio_data_(const uint8_t *data, uint32_t len) {
  if (this->ring_buffer_ == nullptr)
    return;
  this->ring_buffer_->write(data, len);
}

// ---------------------------------------------------------------------------
// AVRCP target callback
// ---------------------------------------------------------------------------

void A2DPSink::handle_avrc_tg_event_(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
  switch (event) {
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
      uint8_t vol = param->set_abs_vol.abs_vol;
      // Acknowledge immediately from BT task (required by spec)
      esp_avrc_rn_param_t rn_param;
      rn_param.volume = vol;
      esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
      // Queue the event for main-loop processing
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

// ---------------------------------------------------------------------------
// AVRCP CT callback handler
// ---------------------------------------------------------------------------

void A2DPSink::handle_avrc_ct_event_(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT: {
      A2DPEventRecord ev{};
      ev.type = param->conn_stat.connected ? A2DPEvent::AVRCP_CT_CONNECTED : A2DPEvent::AVRCP_CT_DISCONNECTED;
      xQueueSend(this->event_queue_, &ev, 0);
      break;
    }
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
      // After receiving PRESSED response, automatically send RELEASED
      if (param->psth_rsp.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
        uint8_t rel_tl = (param->psth_rsp.tl + 1) % 15;
        esp_avrc_ct_send_passthrough_cmd(rel_tl, param->psth_rsp.key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// AVRCP CT passthrough helper
// ---------------------------------------------------------------------------

void A2DPSink::send_avrc_passthrough_(uint8_t key_code) {
  if (!this->avrcp_ct_connected_) {
    ESP_LOGW(TAG, "AVRCP CT not connected — passthrough command ignored");
    return;
  }
  uint8_t tl = this->avrc_ct_tl_;
  this->avrc_ct_tl_ = (this->avrc_ct_tl_ + 2) % 15;  // Skip ahead by 2 (press + release TLs)
  esp_avrc_ct_send_passthrough_cmd(tl, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
}

// ---------------------------------------------------------------------------
// GAP callback — peer name lookup
// ---------------------------------------------------------------------------

void A2DPSink::handle_gap_event_(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT && param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
    A2DPEventRecord ev{};
    ev.type = A2DPEvent::PEER_NAME_UPDATED;
    strncpy(ev.peer_name, reinterpret_cast<const char *>(param->read_rmt_name.rmt_name),
            sizeof(ev.peer_name) - 1);
    ev.peer_name[sizeof(ev.peer_name) - 1] = '\0';
    xQueueSend(this->event_queue_, &ev, 0);
  }
}

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK
