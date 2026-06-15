#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "../a2dp_sink.h"

namespace esphome::a2dp_sink {

/**
 * @brief AVRCP subcomponent for A2DP Sink.
 *
 * Exposes:
 *  - on_volume_changed  trigger  (x: uint8_t, range 0-127)
 *  - a2dp_sink.avrcp.play / .pause / .next / .previous / .stop /
 *    .volume_up / .volume_down  automation actions
 *
 * The volume range follows the Bluetooth AVRCP specification (0-127).
 * To convert to percent in YAML use:  !lambda "return (float)x / 127.0f * 100.0f;"
 */
class A2DPSinkAVRCP : public Component, public Parented<A2DPSink> {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void setup() override {
    this->parent_->add_on_avrcp_volume_callback([this](uint8_t volume) {
      this->volume_callback_.call(volume);
    });
  }

  void dump_config() override {
    static const char *const TAG = "a2dp_sink.avrcp";
    ESP_LOGCONFIG(TAG, "A2DP Sink AVRCP:");
    ESP_LOGCONFIG(TAG, "  CT connected: %s", this->parent_->is_avrcp_ct_connected() ? "yes" : "no");
  }

  // --- Called by build_callback_automation ---
  template<typename F>
  void add_on_volume_callback(F &&callback) {
    this->volume_callback_.add(std::forward<F>(callback));
  }

  // --- Transport control (delegates to A2DPSink AVRCP CT) ---
  void play() { this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_PLAY, true); }
  void pause() { this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_PAUSE); }
  void next_track() { this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_FORWARD, true); }
  void previous_track() { this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_BACKWARD, true); }
  void stop() { this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_PAUSE); }
  void volume_up() { this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_VOL_UP); }
  void volume_down() { this->parent_->get_parent()->send_avrc_passthrough(ESP_AVRC_PT_CMD_VOL_DOWN); }

  /// @brief Current AVRCP absolute volume (0-127) as last reported by remote.
  uint8_t get_volume() const { return this->parent_->get_avrcp_volume(); }

 protected:
  CallbackManager<void(uint8_t)> volume_callback_;
};

// ---------------------------------------------------------------------------
// Automation action classes
// ---------------------------------------------------------------------------

template<typename... Ts>
class A2DPSinkAVRCPPlayAction : public Action<Ts...>, public Parented<A2DPSinkAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->play(); }
};

template<typename... Ts>
class A2DPSinkAVRCPPauseAction : public Action<Ts...>, public Parented<A2DPSinkAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->pause(); }
};

template<typename... Ts>
class A2DPSinkAVRCPNextAction : public Action<Ts...>, public Parented<A2DPSinkAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->next_track(); }
};

template<typename... Ts>
class A2DPSinkAVRCPPreviousAction : public Action<Ts...>, public Parented<A2DPSinkAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->previous_track(); }
};

template<typename... Ts>
class A2DPSinkAVRCPStopAction : public Action<Ts...>, public Parented<A2DPSinkAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts>
class A2DPSinkAVRCPVolumeUpAction : public Action<Ts...>, public Parented<A2DPSinkAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->volume_up(); }
};

template<typename... Ts>
class A2DPSinkAVRCPVolumeDownAction : public Action<Ts...>, public Parented<A2DPSinkAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->volume_down(); }
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK
