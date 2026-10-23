/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <mooncake_log_signal.h>
#include <M5Unified.h>
#include <string>

class CapCC1101 {
public:
    struct cc1101_config {
        static constexpr float freq       = 433.0f;  // default, China band
        static constexpr float bitRate    = 2.4f;    // kbps
        static constexpr float freqOffset = 25.4f;   // kHz frequency deviation
        static constexpr float rxBw       = 58.0f;   // kHz receiver bandwidth
        static constexpr int8_t power     = 10;      // dBm
        static constexpr uint8_t preamble = 16;      // bits
    };

    bool init(float freq = cc1101_config::freq);
    void update();

    bool setFrequency(float freq);
    bool setRfBand(float freq);
    bool sendMsg(const std::string& msg);
    void startReceive();
    float getRSSI();

    // signal: message, rssi
    mclog::Signal<const std::string&, float> onRxMsg;

    inline bool isInited() const
    {
        return _is_inited;
    }
    inline float getFrequency() const
    {
        return _freq;
    }

private:
    bool _is_inited = false;
    float _freq     = cc1101_config::freq;
};
