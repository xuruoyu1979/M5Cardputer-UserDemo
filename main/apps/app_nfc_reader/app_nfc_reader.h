/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <hal/hal.h>
#include <mooncake.h>
#include <cstdint>
#include <string>

/**
 * @brief NFC tag reader app (ST25R3916 on Cap CC1101)
 *
 */
class AppNfcReader : public mooncake::AppAbility {
public:
    AppNfcReader();
    ~AppNfcReader();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    bool _is_cap_available  = false;
    int _tag_slot_id        = -1;
    int _key_slot_id        = -1;
    bool _dump_requested    = false;
    std::string _dump_output;

    void render_screen();
    void request_dump();
    void handle_key_event(const Keyboard::KeyEvent_t& keyEvent);
};
