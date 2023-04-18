#pragma once

#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/lock/lock.h"
#include "esphome/components/output/binary_output.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"


namespace esphome {
namespace yale {

enum EventSource {
    REFRESH,
    AUTO,
    MANUAL,
    MODULE,
    PIN,
    NFC
};


struct YaleLockPacket {
    uint8_t cmd;
    uint8_t src;
    uint8_t count;
    uint8_t size;
    uint8_t payload[36];
};


union YaleLockPacketBuffer {
    YaleLockPacket packet;
    // 40 byte max implied in packet structure, most messages much shorter (discovery = 31b)
    uint8_t raw[40];
};


class YaleSmartLock : public lock::Lock, public Component, public uart::UARTDevice {
public:
    YaleSmartLock(uart::UARTComponent *parent, output::BinaryOutput *wake_output) : UARTDevice(parent), wake_output_(wake_output) {}

    float get_setup_priority() const override { return setup_priority::HARDWARE; }

    void setup() override;
    void loop() override;
    void dump_config() override;

    void set_low_battery_sensor(binary_sensor::BinarySensor *sensor) {
        this->low_battery_sensor_ = sensor;
        this->low_battery_sensor_->publish_state(false);
    }
    void set_event_text_sensor(text_sensor::TextSensor *sensor) { this->event_text_sensor_ = sensor; }

protected:
    // External components
    output::BinaryOutput *wake_output_;
    
    // Optional
    binary_sensor::BinarySensor *low_battery_sensor_{nullptr};
    text_sensor::TextSensor *event_text_sensor_{nullptr};

    // Lock interface
    void control(const lock::LockCall &call) override;

    // Packet serial state
    bool discovered_;
    YaleLockPacketBuffer packet_rx_buffer_;
    uint8_t packet_rx_buffer_idx_;
    uint32_t packet_rx_timeout_start_;
    uint32_t packet_rx_update_start_;

    YaleLockPacketBuffer packet_tx_buffer_;

    // Internal methods
    void publish_event_json_(const json::json_build_t &obj);

    // Handle packet rx/tx
    void handle_packet_();
    void write_packet_();

    // MQTT
    std::string get_topic_for_(const std::string &suffix) const;
};

}  // namespace yale_smart_lock
}  // namespace esphome
