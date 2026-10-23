/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "app_chat/view/chat_view.h"
#include <hal/cap_cc1101/cap_cc1101.h>

class CC1101ChatView : public ChatView {
public:
    static constexpr int FreqPresetCount = 4;
    static constexpr float FreqPresets[FreqPresetCount] = {315.0f, 433.0f, 868.0f, 915.0f};

    ~CC1101ChatView();

    void init();
    void update() override;
    inline float getFrequency() const
    {
        return _freq;
    }

private:
    static constexpr int CC1101_CONFIG_PANEL_HEIGHT = 14;

    float _freq            = CapCC1101::cc1101_config::freq;
    int _freq_index        = 1;  // 433M
    int _ctrl_slot_id      = -1;

    void render_chat_interface() override;
    void render_input_panel() override;
    void render_config_panel();
    void handle_ctrl_key_event(const Keyboard::KeyEvent_t& keyEvent);
    void switch_freq(int delta);
};
