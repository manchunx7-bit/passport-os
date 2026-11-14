#pragma once

#include "apps/app_base.h"

namespace netease {

enum class Page : uint8_t { Message, Login, Lists, Tracks, Player };
enum class PlayerAction : uint8_t { None, Previous, Next, Toggle, VolumeUp, VolumeDown };

// The volume notice is only feedback, never an alternate input mode.
constexpr PlayerAction player_action(uint8_t button, app_btn_event_t event) {
    if (event == BTN_EVT_CLICK) {
        if (button == BSP_BTN_UP) return PlayerAction::Previous;
        if (button == BSP_BTN_DOWN) return PlayerAction::Next;
        if (button == BSP_BTN_OK) return PlayerAction::Toggle;
    } else if (event == BTN_EVT_LONG) {
        if (button == BSP_BTN_UP) return PlayerAction::VolumeUp;
        if (button == BSP_BTN_DOWN) return PlayerAction::VolumeDown;
    }
    return PlayerAction::None;  // long OK is handled by on_back
}

constexpr uint8_t stepped_volume(uint8_t volume, int step) {
    const int value = int(volume) + step;
    return static_cast<uint8_t>(value < 0 ? 0 : value > 100 ? 100 : value);
}

constexpr Page back_page(Page page, bool have_tracks) {
    return page == Page::Player && have_tracks ? Page::Tracks : Page::Lists;
}

}  // namespace netease
