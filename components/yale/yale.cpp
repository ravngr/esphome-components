#include "yale.h"


namespace esphome {
namespace yale {
    static const char *const TAG = "yale.YaleSmartLock";

    static const uint32_t packet_timeout_us = 10000;
    static const uint32_t packet_update_us = 1800000000;
    static const uint8_t packet_minimum_size = 5;

    static const uint8_t packet_src_module = 0xA0;
    static const uint8_t packet_src_lock = 0xB0;


    void YaleSmartLock::setup() {
        // Check UART
        this->check_uart_settings(19200);

        // Setup packet state
        this->discovered_ = false;
        this->packet_rx_buffer_idx_ = 0;

        // Set wake output high
        this->wake_output_->set_state(false);

        // Request status shortly after startup
        this->packet_rx_update_start_ = micros();

        ESP_LOGD(TAG, "setup() done");
    }

    void YaleSmartLock::loop() {
        while (this->available()) {
            // Read byte and increment index
            this->read_byte(&this->packet_rx_buffer_.raw[this->packet_rx_buffer_idx_++]);
            // ESP_LOGD(TAG, "RX 0x%02X", this->packet_rx_buffer_.raw[this->packet_rx_buffer_idx_ - 1]);

            if (this->packet_rx_buffer_idx_ >= 40) {
                ESP_LOGE(TAG, "Packet buffer overflow");
                this->packet_rx_buffer_idx_ = 0;
            } else {
                this->packet_rx_timeout_start_ = micros();
            }
        }

        if (this->packet_rx_buffer_idx_ > 0 && (micros() - this->packet_rx_timeout_start_) > packet_timeout_us) {
            this->handle_packet_();

            // Clear buffer
            this->packet_rx_buffer_idx_ = 0;
        }

        if (this->discovered_ && (micros() - this->packet_rx_update_start_) > packet_update_us) {
            // Request status update
            ESP_LOGD(TAG, "Request update");
            this->packet_tx_buffer_.packet.cmd = 0x11;
            this->packet_tx_buffer_.packet.src = packet_src_module;
            this->packet_tx_buffer_.packet.count = 0;
            this->packet_tx_buffer_.packet.size = 0;
            this->write_packet_();

            this->packet_rx_update_start_ = micros();
        }
    }

    void YaleSmartLock::dump_config() {
        ESP_LOGCONFIG(TAG, "Yale Smart Lock");
    }

    void YaleSmartLock::control(const lock::LockCall &call) {
        auto state = *call.get_state();

        // Prepare packet
        this->packet_tx_buffer_.packet.cmd = 0x10;
        this->packet_tx_buffer_.packet.src = packet_src_module;
        this->packet_tx_buffer_.packet.count = 0;
        this->packet_tx_buffer_.packet.size = 1;

        if (state == lock::LOCK_STATE_LOCKED) {
            this->packet_tx_buffer_.packet.payload[0] = 0xFF;
        } else if (state == lock::LOCK_STATE_UNLOCKED) {
            this->packet_tx_buffer_.packet.payload[0] = 0x00;
        }

        // Send packet
        this->write_packet_();

        // Update after lock reports state change
        // this->publish_state(state);
    }

    void YaleSmartLock::handle_packet_() {
        bool send_ack = false;

        ESP_LOGD(TAG, "Packet len=%d", this->packet_rx_buffer_idx_);
        for (uint8_t n = 0; n < this->packet_rx_buffer_idx_; n++) {
            ESP_LOGD(TAG, "Packet raw[%d] = 0x%02X", n, this->packet_rx_buffer_.raw[n]);
        }

        if (this->packet_rx_buffer_idx_ < packet_minimum_size) {
            ESP_LOGE(
                TAG,
                "Packet too short (%db)",
                this->packet_rx_buffer_idx_
            );
            return;
        }

        // Validate checksum
        uint8_t checksum = 0;
        uint8_t checksum_packet = this->packet_rx_buffer_.raw[this->packet_rx_buffer_idx_ - 1];

        for (uint8_t n = 0; n < (this->packet_rx_buffer_idx_ - 1); n++)
            checksum ^= this->packet_rx_buffer_.raw[n];

        if (checksum != checksum_packet) {
            ESP_LOGE(
                TAG,
                "Packet checksum error (calc: 0x%02X, rx: 0x%02X)",
                checksum,
                checksum_packet
            );
            return;
        }

        if (this->packet_rx_buffer_idx_ != (this->packet_rx_buffer_.packet.size + packet_minimum_size)) {
            ESP_LOGW(
                TAG,
                "Packet size mis-match (buffer: %db, packet: %db + %db)",
                this->packet_rx_buffer_idx_,
                this->packet_rx_buffer_.packet.size,
                packet_minimum_size
            );
            return;
        }

        if (this->packet_rx_buffer_.packet.src != packet_src_lock && (this->packet_rx_buffer_.packet.src != (packet_src_lock | 0x01) || this->packet_rx_buffer_.packet.cmd != 0x11)) {
            ESP_LOGW(
                TAG,
                "Ignoring non-lock packet/NACK (cmd: 0x%02X, src: 0x%02X, count: 0x%02X)",
                this->packet_rx_buffer_.packet.cmd,
                this->packet_rx_buffer_.packet.src,
                this->packet_rx_buffer_.packet.count
            );
            return;
        }

        switch (this->packet_rx_buffer_.packet.cmd) {
        case 0x11:
            // Current state
            if (this->packet_rx_buffer_.packet.payload[0] == 0xFF) {
                ESP_LOGD(TAG, "Status -> locked");
                this->publish_state(lock::LOCK_STATE_LOCKED);
            } else if (this->packet_rx_buffer_.packet.payload[0] == 0x00) {
                ESP_LOGD(TAG, "Status -> unlocked");
                this->publish_state(lock::LOCK_STATE_UNLOCKED);
            }
            break;

        case 0x30:
            // Lock event
            switch (this->packet_rx_buffer_.packet.payload[0]) {
            case 0x09:
                // Jam/stall?
                ESP_LOGE(TAG, "Jammed!");
                
                this->publish_event_json_([=](JsonObject root) {
                    root["state"] = String(lock::lock_state_to_string(lock::LOCK_STATE_JAMMED));
                });

                this->publish_state(lock::LOCK_STATE_JAMMED);
                break;

            case 0x13:
                // PIN unlock
                ESP_LOGD(TAG, "PIN unlock, slot: 0x%02X", this->packet_rx_buffer_.packet.payload[1]);
                
                this->publish_event_json_([=](JsonObject root) {
                    root["state"] = String(lock::lock_state_to_string(lock::LOCK_STATE_UNLOCKED));
                    root["source"] = "pin";
                    root["slot"] = this->packet_rx_buffer_.packet.payload[1];
                });

                this->publish_state(lock::LOCK_STATE_UNLOCKED);
                break;

            case 0x15:
                // Manual lock
                ESP_LOGD(TAG, "External lock");

                this->publish_event_json_([=](JsonObject root) {
                    root["state"] = String(lock::lock_state_to_string(lock::LOCK_STATE_LOCKED));
                    root["source"] = "user";
                });

                this->publish_state(lock::LOCK_STATE_LOCKED);
                break;

            case 0x18:
                // Module lock
                ESP_LOGD(TAG, "Module lock");

                this->publish_event_json_([=](JsonObject root) {
                    root["state"] = String(lock::lock_state_to_string(lock::LOCK_STATE_LOCKED));
                    root["source"] = "self";
                });

                this->publish_state(lock::LOCK_STATE_LOCKED);
                break;

            case 0x19:
                // Module unlock
                ESP_LOGD(TAG, "Module unlock");

                this->publish_event_json_([=](JsonObject root) {
                    root["state"] = String(lock::lock_state_to_string(lock::LOCK_STATE_UNLOCKED));
                    root["source"] = "self";
                });

                this->publish_state(lock::LOCK_STATE_UNLOCKED);
                break;

            case 0x1B:
                // Automatic re-lock
                ESP_LOGD(TAG, "Timeout lock");

                this->publish_event_json_([=](JsonObject root) {
                    root["state"] = String(lock::lock_state_to_string(lock::LOCK_STATE_LOCKED));
                    root["source"] = "timeout";
                });

                this->publish_state(lock::LOCK_STATE_LOCKED);
                break;

            case 0x90:
                // NFC unlock
                ESP_LOGD(TAG, "NFC unlock, slot: 0x%02X", this->packet_rx_buffer_.packet.payload[1]);

                this->publish_event_json_([=](JsonObject root) {
                    root["state"] = String(lock::lock_state_to_string(lock::LOCK_STATE_UNLOCKED));
                    root["source"] = "nfc";
                    root["slot"] = this->packet_rx_buffer_.packet.payload[1];
                });

                this->publish_state(lock::LOCK_STATE_UNLOCKED);
                break;
            
            case 0xA7:
                // Low battery
                ESP_LOGD(TAG, "Low-battery flag");

                if (this->low_battery_sensor_ != nullptr) {
                    this->low_battery_sensor_->publish_state(true);
                }
                break;

            default:
                ESP_LOGE(
                    TAG,
                    "Unknown event (cmd: 0x%02X, count: 0x%02X, size: 0x%02X)",
                    this->packet_rx_buffer_.packet.cmd,
                    this->packet_rx_buffer_.packet.count,
                    this->packet_rx_buffer_.packet.size
                );
            }
            break;

        case 0x33:
            // Setting changed
            ESP_LOGD(TAG, "Setting update");
            send_ack = true;
            break;

        case 0x37:
            // Enter configuration
            ESP_LOGD(TAG, "Config enter");
            break;

        case 0x44:
            // Discovery
            ESP_LOGD(TAG, "Discovery");
            this->packet_tx_buffer_.packet.cmd = this->packet_rx_buffer_.packet.cmd;
            this->packet_tx_buffer_.packet.src = 0xA1;
            this->packet_tx_buffer_.packet.count = this->packet_rx_buffer_.packet.count;
            this->packet_tx_buffer_.packet.size = 1;
            this->packet_tx_buffer_.packet.payload[0] = 0x00;
            this->write_packet_();

            // Request status update asap
            this->packet_rx_update_start_ = 0;

            break;

        default:
            ESP_LOGE(TAG, "Unknown command (0x%02x)", this->packet_rx_buffer_.packet.cmd);
        }

        // Lock is present
        this->discovered_ = true;

        if (send_ack) {
            // Reply with ACK bit set
            this->packet_tx_buffer_.packet.cmd = this->packet_rx_buffer_.packet.cmd;
            this->packet_tx_buffer_.packet.src = 0xA1;
            this->packet_tx_buffer_.packet.count = this->packet_rx_buffer_.packet.count;
            this->packet_tx_buffer_.packet.size = 0;
            this->write_packet_();
        }
    }

    void YaleSmartLock::publish_event_json_(const json::json_build_t &obj) {
        if (this->event_text_sensor_ == nullptr) {
            return;
        }

        std::string json_str = json::build_json(obj);
        this->event_text_sensor_->publish_state(json_str);
    }

    void YaleSmartLock::write_packet_() {
        ESP_LOGD(
            TAG,
            "TX cmd=0x%02X, src=0x%02X, count=0x%02X, length=0x%02X",
            this->packet_tx_buffer_.packet.cmd,
            this->packet_tx_buffer_.packet.src,
            this->packet_tx_buffer_.packet.count,
            this->packet_tx_buffer_.packet.size
        );

        // Calculate checksum
        uint8_t checksum = 0;

        for (uint8_t n = 0; n < (this->packet_tx_buffer_.packet.size + packet_minimum_size - 1); n++)
            checksum ^= this->packet_tx_buffer_.raw[n];

        this->packet_tx_buffer_.packet.payload[this->packet_tx_buffer_.packet.size] = checksum;

        // Pulse wake
        ESP_LOGD(TAG, "Wake LOW");
        this->wake_output_->set_state(true);
        ESP_LOGD(TAG, "Wake HIGH");
        this->wake_output_->set_state(false);

        // Send packet
        this->write_array(this->packet_tx_buffer_.raw, this->packet_tx_buffer_.packet.size + 5);
    }

}  // namespace empty_uart_component
}  // namespace esphome
