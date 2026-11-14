#include "apps/netease/netease_controls.h"
#include <cassert>
#include <cstdio>

using namespace netease;

int main() {
    // The complete physical long-press gesture emits no click on release.
    // Only LONG changes volume; PRESS/LONG_UP/RELEASE never skip a song.
    for (uint8_t button : {BSP_BTN_UP, BSP_BTN_DOWN}) {
        assert(player_action(button, BTN_EVT_PRESS) == PlayerAction::None);
        assert(player_action(button, BTN_EVT_LONG) ==
               (button == BSP_BTN_UP ? PlayerAction::VolumeUp : PlayerAction::VolumeDown));
        assert(player_action(button, BTN_EVT_LONG_UP) == PlayerAction::None);
        assert(player_action(button, BTN_EVT_RELEASE) == PlayerAction::None);
    }
    assert(player_action(BSP_BTN_UP, BTN_EVT_CLICK) == PlayerAction::Previous);
    assert(player_action(BSP_BTN_DOWN, BTN_EVT_CLICK) == PlayerAction::Next);
    assert(player_action(BSP_BTN_OK, BTN_EVT_CLICK) == PlayerAction::Toggle);
    assert(player_action(BSP_BTN_OK, BTN_EVT_LONG) == PlayerAction::None);
    assert(player_action(255, BTN_EVT_CLICK) == PlayerAction::None);

    uint8_t volume = 95;
    volume = stepped_volume(volume, 10); assert(volume == 100);
    volume = stepped_volume(volume, 10); assert(volume == 100);
    volume = stepped_volume(volume, -10); assert(volume == 90);
    assert(stepped_volume(5, -10) == 0 && stepped_volume(0, -10) == 0);
    // A subsequent short press keeps its normal action after volume feedback.
    assert(player_action(BSP_BTN_UP, BTN_EVT_CLICK) == PlayerAction::Previous);
    assert(player_action(BSP_BTN_OK, BTN_EVT_CLICK) == PlayerAction::Toggle);

    Page page = Page::Player;
    page = back_page(page, true); assert(page == Page::Tracks);
    page = back_page(page, true); assert(page == Page::Lists);
    assert(back_page(Page::Player, false) == Page::Lists);
    assert(back_page(Page::Login, false) == Page::Lists);
    assert(back_page(Page::Message, false) == Page::Lists);
    std::puts("NetEase playback keys, volume limits and back hierarchy: PASS");
}
