/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_cc1101_chat.h"
#include "assets/cc1101_big.h"
#include "assets/cc1101_small.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <assets.h>
#include <hal.h>

using namespace mooncake;

static constexpr float _freq_preset[] = {315.0f, 433.0f, 868.0f, 915.0f};

AppCC1101Chat::AppCC1101Chat()
{
    setAppInfo().name     = "CC1101Chat";
    setAppInfo().userData = new AppIcon_t(image_data_cc1101_big, image_data_cc1101_small);
}

AppCC1101Chat::~AppCC1101Chat()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppCC1101Chat::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    // Clear screen
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setCursor(0, 0);

    GetHAL().canvas.setTextColor(TFT_ORANGE);
    GetHAL().canvas.printf("Init Cap CC1101...\n");
    GetHAL().pushCanvas();
    _is_cap_available = GetHAL().capCC1101.init();
    if (!_is_cap_available) {
        mclog::tagError(getAppInfo().name, "cap not available");

        GetHAL().canvas.setTextColor(TFT_ORANGE);
        GetHAL().canvas.printf("Cap init failed.\n");
        GetHAL().canvas.setTextColor(TFT_CYAN);
        GetHAL().canvas.printf("Cap CC1101 ");
        GetHAL().canvas.setTextColor(TFT_WHITE);
        GetHAL().canvas.printf("is required.\nPlease connect it\nto continue.");

        GetHAL().pushCanvas();
        return;
    }

    // Create chat view
    _chat_view            = std::make_unique<CC1101ChatView>();
    _chat_view->onSendMsg = [this](const std::string& message) { send_message(message); };
    _chat_view->init();

    _rx_msg_slot_id = GetHAL().capCC1101.onRxMsg.connect([this](const std::string& message, float rssi) {
        mclog::tagDebug(getAppInfo().name, "received message: \"{}\" rssi: {}", message, rssi);
        if (_chat_view) {
            std::string display = fmt::format("{} ({:.0f}dBm)", message, rssi);
            _chat_view->addMessage(display, false);
        }
    });
}

void AppCC1101Chat::onRunning()
{
    if (_is_cap_available) {
        if (_chat_view) {
            _chat_view->update();
        }
    }

    // Close app when home button clicked
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppCC1101Chat::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    // Clean up chat view
    if (_chat_view) {
        _chat_view.reset();
    }

    if (_rx_msg_slot_id >= 0) {
        GetHAL().capCC1101.onRxMsg.disconnect(_rx_msg_slot_id);
        _rx_msg_slot_id = -1;
    }
}

void AppCC1101Chat::send_message(const std::string& message)
{
    GetHAL().capCC1101.sendMsg(message);
}
