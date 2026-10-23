/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_nfc_reader.h"
#include "assets/nfc_big.h"
#include "assets/nfc_small.h"
#include <apps/utils/audio/audio.h>
#include <apps/utils/common.h>
#include <apps/utils/theme.h>
#include <mooncake_log.h>
#include <assets.h>
#include <hal.h>
#include <fmt/format.h>

using namespace mooncake;

/* Bump this on every debug flash so you can verify the build on screen */
#define NFC_APP_BUILD_TAG "v1.1"

AppNfcReader::AppNfcReader()
{
    setAppInfo().name     = "NFCReader";
    setAppInfo().userData = new AppIcon_t(image_data_nfc_big, image_data_nfc_small);
}

AppNfcReader::~AppNfcReader()
{
    delete static_cast<AppIcon_t*>(getAppInfo().userData);
}

void AppNfcReader::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setCursor(0, 0);

    GetHAL().canvas.setTextColor(TFT_ORANGE);
    GetHAL().canvas.printf("Init NFC (ST25R3916) " NFC_APP_BUILD_TAG "...\n");
    GetHAL().pushCanvas();
    mclog::tagInfo(getAppInfo().name, "build " NFC_APP_BUILD_TAG);

    _is_cap_available = GetHAL().capNFC.init();
    if (!_is_cap_available) {
        mclog::tagError(getAppInfo().name, "cap not available");

        GetHAL().canvas.setTextColor(TFT_ORANGE);
        GetHAL().canvas.printf("NFC init failed.\n");
        GetHAL().canvas.setTextColor(TFT_CYAN);
        GetHAL().canvas.printf("Cap CC1101 ");
        GetHAL().canvas.setTextColor(TFT_WHITE);
        GetHAL().canvas.printf("with NFC\nis required.");

        GetHAL().pushCanvas();
        return;
    }

    _tag_slot_id = GetHAL().capNFC.onTagDetected.connect([this](const std::string& uid, const std::string& type) {
        mclog::tagInfo(getAppInfo().name, "tag: [{}] {}", type, uid);
        audio::play_random_tone();
        _dump_requested = false;
        _dump_output.clear();
        render_screen();
    });

    _key_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_key_event(keyEvent); });

    GetHAL().capNFC.startPolling();
    render_screen();
}

void AppNfcReader::handle_key_event(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyEvent.state || keyEvent.isModifier) {
        return;
    }

    if (keyEvent.keyCode == KEY_ENTER) {
        request_dump();
    }
}


void AppNfcReader::render_screen()
{
    GetHAL().canvas.fillScreen(THEME_COLOR_BG);
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setTextScroll(true);
    GetHAL().canvas.setCursor(0, 0);

    if (GetHAL().capNFC.getLastUid().empty()) {
        GetHAL().canvas.setTextColor(TFT_DARKGRAY);
        GetHAL().canvas.printf("NFC-A polling... [" NFC_APP_BUILD_TAG "]\nApproach a tag.");
        GetHAL().canvas.setTextColor((uint32_t)(0x7FD5CF));
        GetHAL().canvas.println("[Enter] dump tag");
    } else {
        GetHAL().canvas.setTextColor(TFT_CYAN);
        GetHAL().canvas.print("Type: ");
        GetHAL().canvas.setTextColor(TFT_WHITE);
        GetHAL().canvas.println(GetHAL().capNFC.getLastType().c_str());

        GetHAL().canvas.setTextColor(TFT_CYAN);
        GetHAL().canvas.print("UID:  ");
        GetHAL().canvas.setTextColor(TFT_GREEN);
        GetHAL().canvas.println(GetHAL().capNFC.getLastUid().c_str());

        GetHAL().canvas.setTextColor((uint32_t)(0x7FD5CF));
        GetHAL().canvas.println("\n[Enter] dump tag");
    }

    if (_dump_requested && !_dump_output.empty()) {
        GetHAL().canvas.setTextColor(TFT_DARKGRAY);
        GetHAL().canvas.println("--- dump tail ---");
        // Show last ~8 lines of the dump on screen (full data goes to serial log)
        int count = 0;
        size_t pos = _dump_output.size();
        while (pos > 0 && count < 8) {
            pos = _dump_output.rfind('\n', pos == 0 ? 0 : pos - 1);
            if (pos == std::string::npos) {
                pos = 0;
                break;
            }
            count++;
        }
        GetHAL().canvas.print(_dump_output.substr(pos == 0 ? 0 : pos + 1).c_str());
    }

    GetHAL().pushCanvas();
}

void AppNfcReader::request_dump()
{
    if (GetHAL().capNFC.getLastUid().empty()) {
        return;
    }
    _dump_requested = true;
    _dump_output.clear();
    render_screen();

    // Blocking dump; data goes to serial log + screen tail
    std::string out;
    if (GetHAL().capNFC.dumpTag(out)) {
        _dump_output = out;
        mclog::tagInfo(getAppInfo().name, "tag dump:\n{}", out);
    } else {
        _dump_output = "dump failed";
    }
    render_screen();
}

void AppNfcReader::onRunning()
{
    if (_is_cap_available) {
        GetHAL().capNFC.worker();
    }

    // Close app when home button clicked
    if (GetHAL().homeButton.wasClicked()) {
        audio::play_random_tone();
        close();
    }
}

void AppNfcReader::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    if (_tag_slot_id >= 0) {
        GetHAL().capNFC.onTagDetected.disconnect(_tag_slot_id);
        _tag_slot_id = -1;
    }

    if (_key_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_key_slot_id);
        _key_slot_id = -1;
    }

    GetHAL().capNFC.deinit();
}
