#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP) && defined(USE_TEXT_SENSOR)

#include "esphome/components/a2dp/a2dp.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome::a2dp {

class A2DPTextSensor : public text_sensor::TextSensor, public Component, public Parented<A2DP> {
 public:
  void set_metadata_attr(uint8_t metadata_attr) { this->metadata_attr_ = metadata_attr; }

  void setup() override {
    this->parent_->add_on_avrcp_metadata_callback([this](uint8_t metadata_attr, const char *value) {
      if (metadata_attr == this->metadata_attr_) {
        this->publish_state(value);
      }
    });
    this->parent_->add_on_connection_callback([this](bool connected) {
      if (!connected) {
        this->publish_state("");
      }
    });
  }

  void dump_config() override {
    static const char *const TAG = "a2dp.text_sensor";
    LOG_TEXT_SENSOR("", "A2DP Metadata", this);
  }

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

 protected:
  uint8_t metadata_attr_{0};
};

}  // namespace esphome::a2dp

#endif  // USE_ESP32 && USE_A2DP && USE_TEXT_SENSOR
