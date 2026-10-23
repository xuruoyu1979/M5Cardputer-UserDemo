/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "cc1101_chat_view.h"
#include <apps/utils/theme.h>
#include <mooncake_log.h>

static const std::string _tag = "CC1101ChatView";

CC1101ChatView::~CC1101ChatView()
{
    if (_ctrl_slot_id >= 0) {
        GetHAL().keyboard.onKeyEvent.disconnect(_ctrl_slot_id);
        _ctrl_slot_id = -1;
    }
}

void CC1101ChatView::init()
{
    // Find current freq index
    for (int i = 0; i < FreqPresetCount; i++) {
        if (FreqPresets[i] == _freq) {
            _freq_index = i;
            break;
        }
    }

    _ctrl_slot_id = GetHAL().keyboard.onKeyEvent.connect(
        [this](const Keyboard::KeyEvent_t& keyEvent) { handle_ctrl_key_event(keyEvent); });

    ChatView::init();
}

void CC1101ChatView::update()
{
    ChatView::update();
}

void CC1101ChatView::handle_ctrl_key_event(const Keyboard::KeyEvent_t& keyEvent)
{
    if (!keyEvent.state || keyEvent.isModifier) {
        return;
    }

    switch (keyEvent.keyCode) {
        case KEY_RIGHT:
        case KEY_UP:
            switch_freq(1);
            break;

        case KEY_LEFT:
        case KEY_DOWN:
            switch_freq(-1);
            break;

        default:
            break;
    }
}

void CC1101ChatView::switch_freq(int delta)
{
    int new_index = _freq_index + delta;
    if (new_index < 0 || new_index >= FreqPresetCount) {
        return;
    }

    _freq_index = new_index;
    _freq       = FreqPresets[_freq_index];

    mclog::tagDebug(_tag, "switch freq to {} MHz", _freq);
    GetHAL().capCC1101.setFrequency(_freq);

    addMessage(fmt::format("Frequency set to {:.0f} MHz", _freq), true);
}

void CC1101ChatView::render_config_panel()
{
    GetHAL().canvas.fillRect(0, 0, GetHAL().canvas.width(), CC1101_CONFIG_PANEL_HEIGHT, THEME_COLOR_BG);
    GetHAL().canvas.setFont(&fonts::Font0);
    GetHAL().canvas.setTextSize(1);
    GetHAL().canvas.setTextColor((uint32_t)(0xFFFFFF));
    GetHAL().canvas.setTextDatum(textdatum_t::middle_left);

    const uint32_t brick_color = 0x60FFF5;
    std::string buffer;

    GetHAL().canvas.fillRect(2, 5, 3, 4, brick_color);
    buffer = fmt::format("Freq:{:.0f}MHz", _freq);
    GetHAL().canvas.drawString(buffer.c_str(), 9, 7);

    GetHAL().canvas.fillRect(81, 5, 3, 4, brick_color);
    buffer = fmt::format("BR:{}", CapCC1101::cc1101_config::bitRate);
    GetHAL().canvas.drawString(buffer.c_str(), 88, 7);

    GetHAL().canvas.fillRect(132, 5, 3, 4, brick_color);
    buffer = fmt::format("BW:{:.0f}", CapCC1101::cc1101_config::rxBw);
    GetHAL().canvas.drawString(buffer.c_str(), 139, 7);

    GetHAL().canvas.fillRect(170, 5, 3, 4, brick_color);
    buffer = fmt::format("TX:{}dBm", CapCC1101::cc1101_config::power);
    GetHAL().canvas.drawString(buffer.c_str(), 177, 7);

    GetHAL().canvas.setTextDatum(textdatum_t::top_left);
    GetHAL().canvas.setFont(FONT_REPL);
}

void CC1101ChatView::render_input_panel()
{
    render_config_panel();

    // Save cursor position
    int saved_x = GetHAL().canvas.getCursorX();
    int saved_y = GetHAL().canvas.getCursorY();

    const uint32_t input_panel_color = (uint32_t)(0x606060);

    // Clear input panel area below the config panel
    GetHAL().canvas.fillRect(0, CC1101_CONFIG_PANEL_HEIGHT, GetHAL().canvas.width(), INPUT_PANEL_HEIGHT,
                             input_panel_color);

    // Draw prompt and input buffer
    GetHAL().canvas.setTextColor(TFT_WHITE, input_panel_color);
    GetHAL().canvas.setCursor(5, CC1101_CONFIG_PANEL_HEIGHT);
    GetHAL().canvas.setTextSize(1);

    std::string display_text = ">>> " + _input_buffer;
    if (_cursor_state) {
        display_text += "_";
    } else {
        display_text += " ";
    }

    GetHAL().canvas.print(display_text.c_str());

    // Restore chat message cursor position
    GetHAL().canvas.setCursor(saved_x, saved_y);

    GetHAL().pushCanvas();
}

void CC1101ChatView::render_chat_interface()
{
    GetHAL().canvas.fillScreen(THEME_COLOR_BG);
    GetHAL().canvas.setBaseColor(THEME_COLOR_BG);
    GetHAL().canvas.setFont(FONT_REPL);
    GetHAL().canvas.setTextSize(1);

    // Enable text scrolling for chat messages, below the panels
    GetHAL().canvas.setTextScroll(true);

    // Start below the panels
    GetHAL().canvas.setCursor(0, CC1101_CONFIG_PANEL_HEIGHT + INPUT_PANEL_HEIGHT + 2);

    GetHAL().canvas.setTextColor(TFT_DARKGRAY);
    GetHAL().canvas.println("CC1101 chat ready...\n<-/-> switch freq");

    render_input_panel();
}
