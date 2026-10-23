/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "cap_cc1101.h"
#include "../hal_config.h"
#include "radio_lib/EspHal.h"
#include <driver/sdspi_host.h>
#include <driver/gpio.h>
#include <mooncake_log.h>
#include <RadioLib.h>
#include <memory>

static const std::string _tag = "Cap-CC1101";

struct RadioLibData_t {
    std::unique_ptr<EspHal> hal;
    std::unique_ptr<Module> module;
    std::unique_ptr<CC1101> cc1101;

    void reset()
    {
        this->cc1101.reset();
        this->module.reset();
        this->hal.reset();
    }
};

static RadioLibData_t _radio_lib;
static volatile bool _rx_flag = false;
static volatile bool _tx_flag = false;

IRAM_ATTR void _cc1101_set_rx_flag(void)
{
    _rx_flag = true;
}

IRAM_ATTR void _cc1101_set_tx_flag(void)
{
    _tx_flag = true;
}

bool CapCC1101::init(float freq)
{
    mclog::tagInfo(_tag, "init, freq: {} MHz", freq);

    if (_is_inited) {
        return true;
    }

    _freq = freq;

    // RF switch pin (RF_SW0) as plain GPIO output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << HAL_PIN_CC1101_RF_SW0),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Create hal, reuse the HAL-initialized SPI bus
    spi_host_device_t spi_host = SDSPI_DEFAULT_HOST;
    _radio_lib.hal = std::make_unique<EspHal>(HAL_PIN_SPI_SCLK, HAL_PIN_SPI_MISO, HAL_PIN_SPI_MOSI, spi_host, true);

    // Create module (CS, GDO0 as IRQ, no RST, no BUSY)
    _radio_lib.module =
        std::make_unique<Module>(_radio_lib.hal.get(), HAL_PIN_SPI_NSS_GPIO, HAL_PIN_CC1101_GDO0, RADIOLIB_NC,
                                 RADIOLIB_NC);

    // Create CC1101
    _radio_lib.cc1101 = std::make_unique<CC1101>(_radio_lib.module.get());
    int state         = _radio_lib.cc1101->begin(freq, cc1101_config::bitRate, cc1101_config::freqOffset,
                                                 cc1101_config::rxBw, cc1101_config::power, cc1101_config::preamble);
    if (state != RADIOLIB_ERR_NONE) {
        mclog::tagError(_tag, "cc1101 init failed, code {}", state);
        _radio_lib.reset();
        return false;
    }
    mclog::tagInfo(_tag, "cc1101 init success");

    // Band select
    setRfBand(freq);

    // Start receiving
    startReceive();

    _is_inited = true;
    return true;
}

bool CapCC1101::setRfBand(float freq)
{
    // RF switch truth table (RF_SW0 -> G13 GPIO, RF_SW1 -> GDO2 inverted logic)
    // 315M:      SW0=0, SW1=0
    // 433M:      SW0=0, SW1=1
    // 868/915M:  SW0=1, SW1=1
    int rf0 = 0;
    int rf1 = 0;
    if (freq >= 800.0f) {
        rf0 = 1;
        rf1 = 1;
    } else if (freq >= 500.0f) {
        // not a supported band, treat as high band
        rf0 = 1;
        rf1 = 1;
    } else if (freq >= 400.0f) {
        rf0 = 0;
        rf1 = 1;
    } else {
        rf0 = 0;
        rf1 = 0;
    }

    mclog::tagInfo(_tag, "set rf band: {} MHz -> SW0: {} SW1: {}", freq, rf0, rf1);

    // RF_SW0: direct GPIO control
    gpio_set_level((gpio_num_t)HAL_PIN_CC1101_RF_SW0, rf0);

    // RF_SW1: driven by GDO2 through DIO mapping
    if (_radio_lib.cc1101) {
        int state = _radio_lib.cc1101->setDIOMapping(
            2, RADIOLIB_CC1101_GDOX_HW_TO_0 | (rf1 ? RADIOLIB_CC1101_GDO2_INV : RADIOLIB_CC1101_GDO2_NORM));
        if (state != RADIOLIB_ERR_NONE) {
            mclog::tagError(_tag, "set DIO mapping failed, code {}", state);
            return false;
        }
    }

    return true;
}

bool CapCC1101::setFrequency(float freq)
{
    if (!_radio_lib.cc1101) {
        return false;
    }

    int state = _radio_lib.cc1101->setFrequency(freq);
    if (state != RADIOLIB_ERR_NONE) {
        mclog::tagError(_tag, "set frequency failed, code {}", state);
        return false;
    }

    _freq = freq;
    setRfBand(freq);
    return true;
}

void CapCC1101::startReceive()
{
    if (!_radio_lib.cc1101) {
        return;
    }

    _radio_lib.cc1101->setPacketReceivedAction(_cc1101_set_rx_flag);
    int state = _radio_lib.cc1101->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        mclog::tagError(_tag, "start receive failed, code {}", state);
    }
}

bool CapCC1101::sendMsg(const std::string& msg)
{
    if (!_is_inited || !_radio_lib.cc1101) {
        return false;
    }

    mclog::tagDebug(_tag, "send msg: {}", msg);

    _radio_lib.cc1101->setPacketSentAction(_cc1101_set_tx_flag);
    int state = _radio_lib.cc1101->startTransmit(msg.c_str());
    if (state != RADIOLIB_ERR_NONE) {
        mclog::tagError(_tag, "send msg failed, code {}", state);
        return false;
    }

    // Wait for transmit to finish, then go back to receiving
    _radio_lib.cc1101->finishTransmit();
    startReceive();
    return true;
}

float CapCC1101::getRSSI()
{
    if (!_radio_lib.cc1101) {
        return 0.0f;
    }
    return _radio_lib.cc1101->getRSSI();
}

void CapCC1101::update()
{
    if (!_is_inited) {
        return;
    }

    if (_rx_flag) {
        _rx_flag = false;

        size_t len = _radio_lib.cc1101->getPacketLength();
        std::string msg;
        msg.resize(len);
        int state = _radio_lib.cc1101->readData(reinterpret_cast<uint8_t*>(&msg[0]), len);
        if (state == RADIOLIB_ERR_NONE) {
            float rssi = _radio_lib.cc1101->getRSSI();
            mclog::tagDebug(_tag, "rx msg: {} | len: {} | RSSI: {}dBm", msg, len, rssi);
            onRxMsg.emit(msg, rssi);
        } else {
            mclog::tagError(_tag, "read data failed, code {}", state);
        }

        // Re-arm receive
        startReceive();
    }

    if (_tx_flag) {
        _tx_flag = false;
        mclog::tagDebug(_tag, "tx done");
    }
}
