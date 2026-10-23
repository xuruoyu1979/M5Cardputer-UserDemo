/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "view/cc1101_chat_view.h"
#include <mooncake.h>
#include <cstdint>
#include <memory>

/**
 * @brief CC1101 RF chat app (Cap CC1101)
 *
 */
class AppCC1101Chat : public mooncake::AppAbility {
public:
    AppCC1101Chat();
    ~AppCC1101Chat();

    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    std::unique_ptr<CC1101ChatView> _chat_view;
    bool _is_cap_available    = false;
    int _rx_msg_slot_id = -1;

    void send_message(const std::string& message);
};
