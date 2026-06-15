#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_A2DP_SINK) && defined(USE_TEXT_SENSOR)

#include "esphome/components/a2dp/a2dp.h"
#include "esphome/components/a2dp_sink/a2dp_sink.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome::a2dp_sink {

/// @brief Text sensor that reports the name of the connected Bluetooth source device.
class A2DPSinkTextSensor : public text_sensor::TextSensor,
                           public Component,
                           public Parented<A2DPSink> {
 public:
  void setup() override {
    this->parent_->get_parent()->add_on_peer_name_callback([this](const char *name) {
      this->publish_state(name);
    });
    this->parent_->get_parent()->add_on_connection_callback([this](bool connected) {
      if (!connected) {
        this->publish_state("");
      }
    });
  }

  void dump_config() override {
    static const char *const TAG = "a2dp_sink.text_sensor";
    LOG_TEXT_SENSOR("", "A2DP Sink Peer Name", this);
  }

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }
};

}  // namespace esphome::a2dp_sink

#endif  // USE_ESP32 && USE_A2DP_SINK && USE_TEXT_SENSOR
