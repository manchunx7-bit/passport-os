// Xiaozhi companion face: capsule eyes that blink, glance and change shape
// with the conversation state and the server's llm.emotion.
//
// Pure drawing + a small frame animator. No retained LVGL objects, images or
// heap of its own: the caller owns one LVGL timer and invalidates its two
// hosts only when the returned Pose changes.
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "apps/xiaozhi/xiaozhi_level.h"

namespace xiaozhi::face {

enum class Activity : uint8_t { Idle, Listening, Thinking, Speaking, Connecting, Offline, Error };

enum class Mood : uint8_t {
    Neutral, Happy, Laugh, Sad, Cry, Angry, Surprised, Love, Wink, Cool, Sleepy, Confused, Shy,
};

// Xiaozhi server emotion names ("happy", "laughing", "crying"...). Unknown -> Neutral.
Mood mood_from_emotion(const char *emotion);

struct Palette {
    uint32_t shell;   // head
    uint32_t edge;    // head outline, ears, shadow
    uint32_t screen;  // face visor behind the eyes
    uint32_t eye;     // eyes and mouth
    uint32_t glint;   // eye highlights
    uint32_t blush;   // cheeks, hearts, tongue
    uint32_t tear;    // tears, sweat, listening light
    uint32_t warn;    // thinking light and bubbles
};

struct Pose {
    Activity activity;
    Mood mood;
    int8_t bob;      // head offset, px down
    int8_t gaze_x;   // eye offset, px
    int8_t gaze_y;
    uint8_t open;    // eye openness 0..100
    uint8_t mouth;   // talking mouth height, 0 = closed
    uint8_t phase;   // decorative loop 0..15
    uint8_t bars[kWaveBars]{}; // recent real audio amplitudes, oldest first
    bool operator==(const Pose &o) const {
        for (int i = 0; i < kWaveBars; ++i) if (bars[i] != o.bars[i]) return false;
        return activity == o.activity && mood == o.mood && bob == o.bob && gaze_x == o.gaze_x &&
               gaze_y == o.gaze_y && open == o.open && mouth == o.mouth && phase == o.phase;
    }
    bool operator!=(const Pose &o) const { return !(*this == o); }
};

constexpr int kFramePeriodMs = 50;

class Animator {
public:
    // An emotion lasts through the reply, then ~6 s of idle afterglow.
    void set_mood(Mood mood);
    Pose step(Activity activity);  // advance one frame

private:
    uint32_t random();

    uint32_t tick_ = 0;
    uint32_t rng_ = 0x9E3779B9u;
    uint32_t blink_at_ = 30;
    uint32_t glance_at_ = 50;
    uint8_t blink_ = 0;  // frames into a blink; 0 = eyes open
    bool blink_twice_ = false;
    int8_t gaze_x_ = 0, gaze_y_ = 0, target_x_ = 0, target_y_ = 0;
    uint8_t mouth_ = 0;
    Mood mood_ = Mood::Neutral;
    uint16_t mood_ttl_ = 0;
    Activity last_ = Activity::Idle;
};

constexpr int kBigW = 124, kBigH = 112;
constexpr int kMiniW = 26, kMiniH = 20;

void draw_big(lv_layer_t *layer, const lv_area_t &area, const Pose &pose, const Palette &palette);
// frame: the status colour that the old square badge carried.
void draw_mini(lv_layer_t *layer, const lv_area_t &area, const Pose &pose, const Palette &palette,
               uint32_t frame);

}  // namespace xiaozhi::face
