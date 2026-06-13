#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_AVRCP)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/a2dp/a2dp.h"

namespace esphome::a2dp {

/**
 * @brief AVRCP controller/target subcomponent — Parented<A2DP>.
 *
 * Registers callbacks on the hub for volume changes and exposes
 * transport control actions (play/pause/next/previous/stop/vol±).
 *
 * Volume range: 0-127 (Bluetooth AVRCP spec).
 */
class A2DPAVRCP : public Component, public Parented<A2DP> {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void setup() override {
    this->parent_->add_on_avrcp_volume_callback([this](uint8_t volume) {
      this->volume_callback_.call(volume);
    });
  }

  void dump_config() override {
    static const char *const TAG = "a2dp_avrcp";
    ESP_LOGCONFIG(TAG, "A2DP AVRCP:");
    ESP_LOGCONFIG(TAG, "  CT connected: %s", this->parent_->is_avrcp_ct_connected() ? "yes" : "no");
  }

  template<typename F>
  void add_on_volume_callback(F &&callback) {
    this->volume_callback_.add(std::forward<F>(callback));
  }

  void play()           { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_PLAY); }
  void pause()          { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_PAUSE); }
  void play_pause() {
    this->parent_->send_avrc_passthrough(
        this->parent_->is_audio_streaming() ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
  }
  void next_track()     { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_FORWARD); }
  void previous_track() { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_BACKWARD); }
  void stop()           { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_STOP); }
  void volume_up()      { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_VOL_UP); }
  void volume_down()    { this->parent_->send_avrc_passthrough(ESP_AVRC_PT_CMD_VOL_DOWN); }

  uint8_t get_volume() const { return this->parent_->get_avrcp_volume(); }

  CallbackManager<void(uint8_t)> volume_callback_;
};

// ---------------------------------------------------------------------------
// Automation action classes
// ---------------------------------------------------------------------------

template<typename... Ts>
class A2DPAVRCPPlayAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->play(); }
};

template<typename... Ts>
class A2DPAVRCPPauseAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->pause(); }
};

template<typename... Ts>
class A2DPAVRCPPlayPauseAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->play_pause(); }
};

template<typename... Ts>
class A2DPAVRCPNextAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->next_track(); }
};

template<typename... Ts>
class A2DPAVRCPPreviousAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->previous_track(); }
};

template<typename... Ts>
class A2DPAVRCPStopAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts>
class A2DPAVRCPVolumeUpAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->volume_up(); }
};

template<typename... Ts>
class A2DPAVRCPVolumeDownAction : public Action<Ts...>, public Parented<A2DPAVRCP> {
 public:
  void play(const Ts &...x) override { this->parent_->volume_down(); }
};

}  // namespace esphome::a2dp

#endif  // USE_ESP32 && USE_A2DP_AVRCP
