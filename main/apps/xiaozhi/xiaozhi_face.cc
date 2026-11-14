// Xiaozhi companion face. Geometry is in the 124x112 (big) and 26x20 (mini)
// host boxes; every shape follows the pose's head bob.
#include "apps/xiaozhi/xiaozhi_face.h"

#include <cstring>

namespace xiaozhi::face {

namespace {

constexpr uint16_t kMoodAfterglowFrames = 6000 / kFramePeriodMs;
constexpr uint8_t kBlinkOpen[] = {100, 55, 10, 10, 55};  // indexed by frames into a blink

struct Pen {
    lv_layer_t *layer;
    int32_t ox, oy;

    void rect(int x, int y, int w, int h, uint32_t color, int radius = 0, lv_opa_t opa = LV_OPA_COVER) {
        if (w <= 0 || h <= 0) return;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(color);
        d.bg_opa = opa;
        d.radius = radius;
        const lv_area_t a{ox + x, oy + y, ox + x + w - 1, oy + y + h - 1};
        lv_draw_rect(layer, &d, &a);
    }
    void panel(int x, int y, int w, int h, uint32_t fill, uint32_t edge, int radius, int border) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(fill);
        d.bg_opa = LV_OPA_COVER;
        d.border_color = lv_color_hex(edge);
        d.border_width = border;
        d.border_opa = LV_OPA_COVER;
        d.radius = radius;
        const lv_area_t a{ox + x, oy + y, ox + x + w - 1, oy + y + h - 1};
        lv_draw_rect(layer, &d, &a);
    }
    void ring(int x, int y, int w, int h, uint32_t color, int radius, int width) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = lv_color_hex(color);
        d.border_width = width;
        d.border_opa = LV_OPA_COVER;
        d.radius = radius;
        const lv_area_t a{ox + x, oy + y, ox + x + w - 1, oy + y + h - 1};
        lv_draw_rect(layer, &d, &a);
    }
    // Angles: 0 = 3 o'clock, 90 = 6 o'clock, drawn clockwise.
    void arc(int cx, int cy, int radius, int width, int start, int end, uint32_t color,
             lv_opa_t opa = LV_OPA_COVER) {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = lv_color_hex(color);
        d.width = width;
        d.start_angle = start;
        d.end_angle = end;
        d.center.x = ox + cx;
        d.center.y = oy + cy;
        d.radius = static_cast<uint16_t>(radius);
        d.opa = opa;
        d.rounded = 1;
        lv_draw_arc(layer, &d);
    }
    void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color) {
        lv_draw_triangle_dsc_t d;
        lv_draw_triangle_dsc_init(&d);
        d.p[0].x = ox + x0; d.p[0].y = oy + y0;
        d.p[1].x = ox + x1; d.p[1].y = oy + y1;
        d.p[2].x = ox + x2; d.p[2].y = oy + y2;
        d.color = lv_color_hex(color);
        d.opa = LV_OPA_COVER;
        lv_draw_triangle(layer, &d);
    }
    void line(int x0, int y0, int x1, int y1, int width, uint32_t color, lv_opa_t opa = LV_OPA_COVER) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.p1.x = ox + x0; d.p1.y = oy + y0;
        d.p2.x = ox + x1; d.p2.y = oy + y1;
        d.width = width;
        d.color = lv_color_hex(color);
        d.opa = opa;
        d.round_start = 1;
        d.round_end = 1;
        lv_draw_line(layer, &d);
    }
};

enum class Eyes : uint8_t { Open, Happy, Closed, Round, Heart, Sad, Angry, Sleepy, Wink, Shades, Squint };
enum class Mouth : uint8_t { Smile, Grin, Laugh, Talk, O, Flat, Frown, Smirk, Wavy, Tiny };

struct Look {
    Eyes eyes;
    Mouth mouth;
    uint8_t blush;  // 0 none, 1 soft, 2 shy
    bool tears;
};

Look look_for(const Pose &p) {
    switch (p.activity) {
    case Activity::Offline:    return {Eyes::Closed, Mouth::Tiny, 0, false};
    case Activity::Error:      return {Eyes::Sad, Mouth::Frown, 0, false};
    case Activity::Connecting: return {Eyes::Open, Mouth::Flat, 0, false};
    case Activity::Listening:  return {Eyes::Open, Mouth::O, 1, false};
    case Activity::Thinking:   return {Eyes::Open, Mouth::Flat, 0, false};
    default: break;
    }
    const bool talking = p.activity == Activity::Speaking;
    const Mouth talk = talking ? Mouth::Talk : Mouth::Smile;
    switch (p.mood) {
    case Mood::Happy:     return {Eyes::Happy, talking ? Mouth::Talk : Mouth::Grin, 1, false};
    case Mood::Laugh:     return {Eyes::Happy, Mouth::Laugh, 1, false};
    case Mood::Sad:       return {Eyes::Sad, talking ? Mouth::Talk : Mouth::Frown, 0, false};
    case Mood::Cry:       return {Eyes::Sad, talking ? Mouth::Talk : Mouth::Frown, 0, true};
    case Mood::Angry:     return {Eyes::Angry, talking ? Mouth::Talk : Mouth::Frown, 0, false};
    case Mood::Surprised: return {Eyes::Round, Mouth::O, 0, false};
    case Mood::Love:      return {Eyes::Heart, talk, 1, false};
    case Mood::Wink:      return {Eyes::Wink, talking ? Mouth::Talk : Mouth::Grin, 1, false};
    case Mood::Cool:      return {Eyes::Shades, talking ? Mouth::Talk : Mouth::Smirk, 0, false};
    case Mood::Sleepy:    return {Eyes::Sleepy, talking ? Mouth::Talk : Mouth::Tiny, 0, false};
    case Mood::Confused:  return {Eyes::Squint, talking ? Mouth::Talk : Mouth::Wavy, 0, false};
    case Mood::Shy:       return {Eyes::Happy, talking ? Mouth::Talk : Mouth::Smile, 2, false};
    default:              return {Eyes::Open, talk, 1, false};
    }
}

int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- big face --------------------------------------------------------------

constexpr int kCx = 62;
constexpr int kEyeY = 52, kEyeLx = 47, kEyeRx = 77;
constexpr int kMouthY = 70;

void open_eye(Pen &pen, int cx, int cy, int w, int h, uint8_t open, const Palette &c) {
    const int eh = h * open / 100 < 3 ? 3 : h * open / 100;
    const int x = cx - w / 2, y = cy - eh / 2;
    pen.rect(x, y, w, eh, c.eye, (w < eh ? w : eh) / 2);
    if (eh >= 12) {
        pen.rect(x + w - 8, y + 3, 5, 6, c.glint, 3);
        pen.rect(x + 3, y + eh - 7, 3, 3, c.glint, 2, LV_OPA_60);
    }
}

void happy_eye(Pen &pen, int cx, int cy, const Palette &c) {
    pen.arc(cx, cy + 6, 9, 4, 200, 340, c.eye);
}

void heart(Pen &pen, int cx, int cy, int s, uint32_t color, uint32_t glint) {
    const int r = 5 + s;
    pen.rect(cx - 5 - r, cy - 3 - r, 2 * r, 2 * r, color, r);
    pen.rect(cx + 5 - r, cy - 3 - r, 2 * r, 2 * r, color, r);
    pen.tri(cx - 10 - s, cy - 1, cx + 10 + s, cy - 1, cx, cy + 10 + s, color);
    pen.rect(cx - 8, cy - 7, 3, 3, glint, 2);
}

// Eyelid wedge in the visor colour: sad lowers the outer corner, angry the inner.
void lid(Pen &pen, int cx, int cy, bool lower_left, const Palette &c) {
    const int x0 = cx - 11, x1 = cx + 11, y0 = cy - 14, drop = cy - 1;
    pen.tri(x0, y0, x1, y0, lower_left ? x0 : x1, drop, c.screen);
}

void draw_eyes(Pen &pen, const Pose &p, const Look &look, int bob, const Palette &c) {
    const bool listening = p.activity == Activity::Listening;
    const int w = listening ? 18 : 16, h = listening ? 25 : 22;
    const int ly = kEyeY + bob + p.gaze_y;
    const int lx = kEyeLx + p.gaze_x, rx = kEyeRx + p.gaze_x;
    switch (look.eyes) {
    case Eyes::Open:
        open_eye(pen, lx, ly, w, h, p.open, c);
        open_eye(pen, rx, ly, w, h, p.open, c);
        break;
    case Eyes::Happy:
        happy_eye(pen, lx, ly, c);
        happy_eye(pen, rx, ly, c);
        break;
    case Eyes::Closed:
        pen.arc(lx, ly - 5, 8, 3, 25, 155, c.eye);
        pen.arc(rx, ly - 5, 8, 3, 25, 155, c.eye);
        break;
    case Eyes::Round:
        open_eye(pen, lx, ly, 21, 21, p.open, c);
        open_eye(pen, rx, ly, 21, 21, p.open, c);
        break;
    case Eyes::Heart: {
        const int s = (p.phase & 7) < 2 ? 1 : 0;  // heartbeat
        heart(pen, lx, ly, s, c.blush, c.glint);
        heart(pen, rx, ly, s, c.blush, c.glint);
        break;
    }
    case Eyes::Sad:
    case Eyes::Angry: {
        const bool sad = look.eyes == Eyes::Sad;
        open_eye(pen, lx, ly, w, h, p.open, c);
        open_eye(pen, rx, ly, w, h, p.open, c);
        lid(pen, lx, ly, sad, c);
        lid(pen, rx, ly, !sad, c);
        break;
    }
    case Eyes::Sleepy:
        open_eye(pen, lx, ly, w, h, p.open, c);
        open_eye(pen, rx, ly, w, h, p.open, c);
        pen.rect(lx - 10, ly - 13, 20, 14, c.screen);
        pen.rect(rx - 10, ly - 13, 20, 14, c.screen);
        pen.rect(lx - 9, ly, 18, 2, c.eye, 1);
        pen.rect(rx - 9, ly, 18, 2, c.eye, 1);
        break;
    case Eyes::Wink:
        open_eye(pen, lx, ly, w, h, p.open, c);
        happy_eye(pen, rx, ly, c);
        break;
    case Eyes::Shades:
        pen.rect(lx - 13, ly - 8, 26, 16, c.eye, 6);
        pen.rect(rx - 13, ly - 8, 26, 16, c.eye, 6);
        pen.rect(lx + 10, ly - 6, rx - lx - 20, 3, c.eye);
        pen.line(lx + 2, ly - 4, lx + 7, ly - 4, 2, c.glint, LV_OPA_80);
        pen.line(rx + 2, ly - 4, rx + 7, ly - 4, 2, c.glint, LV_OPA_80);
        break;
    case Eyes::Squint:
        open_eye(pen, lx, ly, w, h, p.open, c);
        open_eye(pen, rx + 1, ly + 3, 14, 10, 100, c);
        pen.rect(rx - 9, ly - 8, 18, 3, c.eye, 1);  // raised brow
        break;
    }
    if (look.tears) {
        const int fall = (p.phase & 7) * 2;
        pen.rect(lx - 9, ly + 8 + fall, 5, 7, c.tear, 3);
        pen.rect(rx + 4, ly + 8 + ((fall + 6) & 15), 5, 7, c.tear, 3);
    }
}

void draw_mouth(Pen &pen, const Pose &p, const Look &look, int bob, const Palette &c) {
    const int mx = kCx + p.gaze_x / 2 + (p.activity == Activity::Thinking ? 6 : 0);
    const int my = kMouthY + bob + (p.gaze_y > 0 ? 1 : 0);
    switch (look.mouth) {
    case Mouth::Smile: pen.arc(mx, my - 6, 7, 3, 25, 155, c.eye); break;
    case Mouth::Grin:  pen.arc(mx, my - 8, 9, 3, 15, 165, c.eye); break;
    case Mouth::Laugh: {
        const int h = 9 + (p.mouth > 4 ? 2 : 0);
        pen.rect(mx - 9, my - 3, 18, h, c.eye, 6);
        pen.rect(mx - 9, my - 3, 18, 4, c.eye, 1);
        pen.rect(mx - 5, my + h - 7, 10, 4, c.blush, 2);
        break;
    }
    case Mouth::Talk: {
        // Open mouth: lip ring, dark cavity, tongue once it is wide enough.
        const int h = clamp(p.mouth, 2, 11), w = 12 + h / 2;
        const int x = mx - w / 2, y = my - h / 2;
        pen.rect(x, y, w, h, c.eye, (h < w ? h : w) / 2);
        if (h >= 6) {
            pen.rect(x + 2, y + 2, w - 4, h - 4, c.screen, (h - 4) / 2);
            pen.rect(x + 4, y + h - 5, w - 8, 3, c.blush, 2);
        }
        break;
    }
    case Mouth::O: {
        const int grow = p.activity == Activity::Listening ? ((p.phase >> 2) & 1) : 2;
        pen.ring(mx - 4 - grow, my - 5 - grow, 8 + 2 * grow, 10 + 2 * grow, c.eye, 5 + grow, 3);
        break;
    }
    case Mouth::Flat:  pen.rect(mx - 6, my - 1, 12, 3, c.eye, 1); break;
    case Mouth::Frown: pen.arc(mx, my + 7, 7, 3, 205, 335, c.eye); break;
    case Mouth::Smirk: pen.arc(mx + 2, my - 6, 7, 3, 15, 95, c.eye); break;
    case Mouth::Wavy:
        for (int i = 0; i < 4; ++i) pen.line(mx - 8 + i * 4, my - (i & 1 ? -1 : 1), mx - 4 + i * 4, my + (i & 1 ? -1 : 1), 2, c.eye);
        break;
    case Mouth::Tiny:  pen.rect(mx - 3, my - 1, 6, 3, c.eye, 1); break;
    }
}

void draw_letter_z(Pen &pen, int x, int y, int s, uint32_t color, lv_opa_t opa) {
    pen.line(x, y, x + s, y, 2, color, opa);
    pen.line(x + s, y, x, y + s, 2, color, opa);
    pen.line(x, y + s, x + s, y + s, 2, color, opa);
}

void draw_extras(Pen &pen, const Pose &p, int bob, const Palette &c) {
    switch (p.activity) {
    case Activity::Listening: {
        // Sound ripples beside the ears, stepping outward.
        const int step = (p.phase >> 1) % 3;
        for (int i = 0; i < 3; ++i) {
            const lv_opa_t opa = i == step ? LV_OPA_COVER : LV_OPA_30;
            const int r = 53 + i * 4;
            pen.arc(kCx, 55 + bob, r, 2, 346, 14, c.tear, opa);
            pen.arc(kCx, 55 + bob, r, 2, 166, 194, c.tear, opa);
        }
        break;
    }
    case Activity::Thinking: {
        const int shown = (p.phase >> 2) & 3;
        if (shown > 0) pen.rect(94, 15, 5, 5, c.warn, 3);
        if (shown > 1) pen.rect(101, 8, 7, 7, c.warn, 4);
        if (shown > 2) pen.rect(110, 1, 10, 10, c.warn, 5);
        break;
    }
    case Activity::Offline: {
        const int rise = p.phase >> 2;
        draw_letter_z(pen, 92, 14 - rise, 6, c.edge, LV_OPA_COVER);
        draw_letter_z(pen, 103, 6 - rise / 2, 8, c.edge, rise > 2 ? LV_OPA_50 : LV_OPA_COVER);
        break;
    }
    case Activity::Error: {
        const int slide = (p.phase & 7) / 2;
        pen.tri(100, 23 + slide + bob, 96, 31 + slide + bob, 104, 31 + slide + bob, c.tear);
        pen.rect(96, 27 + slide + bob, 9, 9, c.tear, 5);
        break;
    }
    default: break;
    }
}

uint32_t antenna_color(const Pose &p, const Palette &c) {
    switch (p.activity) {
    case Activity::Listening: return c.tear;
    case Activity::Thinking:
    case Activity::Connecting: return (p.phase & 2) ? c.warn : c.edge;
    case Activity::Offline: return c.edge;
    default: return c.eye;
    }
}

}  // namespace

Mood mood_from_emotion(const char *e) {
    if (!e) return Mood::Neutral;
    struct Entry { const char *name; Mood mood; };
    static constexpr Entry kTable[] = {
        {"happy", Mood::Happy},       {"relaxed", Mood::Happy},     {"confident", Mood::Happy},
        {"delicious", Mood::Happy},   {"funny", Mood::Laugh},       {"silly", Mood::Laugh},
        {"laughing", Mood::Laugh},    {"sad", Mood::Sad},           {"crying", Mood::Cry},
        {"angry", Mood::Angry},       {"surprised", Mood::Surprised}, {"shocked", Mood::Surprised},
        {"loving", Mood::Love},       {"kissy", Mood::Love},        {"winking", Mood::Wink},
        {"cool", Mood::Cool},         {"sleepy", Mood::Sleepy},     {"thinking", Mood::Confused},
        {"confused", Mood::Confused}, {"embarrassed", Mood::Shy},
    };
    for (const auto &entry : kTable) {
        if (std::strcmp(e, entry.name) == 0) return entry.mood;
    }
    return Mood::Neutral;
}

uint32_t Animator::random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

void Animator::set_mood(Mood mood) {
    mood_ = mood;
    mood_ttl_ = kMoodAfterglowFrames;
}

Pose Animator::step(Activity a) {
    ++tick_;
    if (a != last_) {
        last_ = a;
        if (a == Activity::Listening || a == Activity::Connecting || a == Activity::Offline ||
            a == Activity::Error) mood_ = Mood::Neutral;
        glance_at_ = tick_;
    }
    if (a == Activity::Idle && mood_ != Mood::Neutral && mood_ttl_ && --mood_ttl_ == 0) mood_ = Mood::Neutral;

    // Where the eyes want to look.
    switch (a) {
    case Activity::Thinking:
        target_x_ = (tick_ / 12) & 1 ? 4 : 5;
        target_y_ = -4;
        break;
    case Activity::Connecting: {
        const int t = tick_ % 32;
        target_x_ = static_cast<int8_t>((t < 16 ? t : 32 - t) * 10 / 16 - 5);
        target_y_ = 0;
        break;
    }
    case Activity::Listening: target_x_ = 0; target_y_ = 0; break;
    case Activity::Offline:   target_x_ = 0; target_y_ = 2; break;
    case Activity::Error:     target_x_ = 0; target_y_ = 1; break;
    default:
        if (tick_ >= glance_at_) {
            static constexpr int8_t kGlances[][2] = {
                {0, 0}, {0, 0}, {0, 0}, {-5, 0}, {5, 0}, {-4, -3}, {4, -3}, {0, 2}, {-3, 1}, {3, 1},
            };
            const auto &g = kGlances[random() % (sizeof(kGlances) / sizeof(kGlances[0]))];
            const bool calm = a == Activity::Speaking;
            target_x_ = calm ? g[0] / 2 : g[0];
            target_y_ = calm ? g[1] / 2 : g[1];
            glance_at_ = tick_ + 30 + random() % 60;
        }
        if (mood_ == Mood::Shy) target_y_ = 3;
        break;
    }
    // Quick saccade: up to 3 px per frame.
    gaze_x_ = static_cast<int8_t>(gaze_x_ + clamp(target_x_ - gaze_x_, -3, 3));
    gaze_y_ = static_cast<int8_t>(gaze_y_ + clamp(target_y_ - gaze_y_, -3, 3));

    // Blink every 2.5-6 s, sometimes twice.
    uint8_t open = 100;
    if (a != Activity::Offline) {
        if (!blink_ && tick_ >= blink_at_) blink_ = 1;
        if (blink_) {
            open = kBlinkOpen[blink_];
            if (++blink_ >= sizeof(kBlinkOpen)) {
                blink_ = 0;
                if (blink_twice_) {
                    blink_twice_ = false;
                    blink_at_ = tick_ + 3;
                } else {
                    blink_twice_ = random() % 4 == 0;
                    blink_at_ = tick_ + 50 + random() % 70;
                }
            }
        }
    }

    if (a == Activity::Speaking) {
        if (tick_ % 2 == 0) mouth_ = random() % 6 == 0 ? 2 : static_cast<uint8_t>(3 + random() % 8);
    } else {
        mouth_ = 0;
    }

    static constexpr int8_t kBob[] = {0, 0, 1, 2, 2, 1};
    int8_t bob = 0;
    uint8_t phase = 0;
    switch (a) {
    case Activity::Idle:       bob = kBob[(tick_ / 8) % 6]; break;
    case Activity::Speaking:   bob = kBob[(tick_ / 4) % 6]; break;
    case Activity::Thinking:   bob = kBob[(tick_ / 10) % 6]; phase = (tick_ / 5) & 15; break;
    case Activity::Connecting: bob = kBob[(tick_ / 6) % 6]; phase = (tick_ / 4) & 15; break;
    case Activity::Offline:    bob = kBob[(tick_ / 14) % 6]; phase = (tick_ / 5) & 15; break;
    case Activity::Listening:  phase = (tick_ / 3) & 15; break;
    case Activity::Error:      phase = (tick_ / 3) & 15; break;
    }
    if (mood_ == Mood::Love || mood_ == Mood::Cry) phase = (tick_ / 2) & 15;

    return Pose{a, mood_, bob, gaze_x_, gaze_y_, open, mouth_, phase};
}

void draw_big(lv_layer_t *layer, const lv_area_t &area, const Pose &p, const Palette &c) {
    Pen pen{layer, area.x1, area.y1};
    const Look look = look_for(p);
    const int b = p.bob;

    // A compact amplitude history lives below the floating head. Silent columns
    // rest as quiet dots; it fits the existing host without stealing text space.
    for (int i = 0; i < kWaveBars; ++i) {
        const int height = 2 + (p.bars[i] * 12 + 50) / 100;
        pen.rect(kCx - 38 + i * 6, 103 - height / 2, 4, height,
                 p.bars[i] ? c.eye : c.edge, 2, p.bars[i] ? LV_OPA_COVER : LV_OPA_50);
    }

    // Antenna, then ears, so the head overlaps their roots.
    pen.rect(kCx - 2, 9 + b, 4, 12, c.edge, 2);
    const uint32_t tip = antenna_color(p, c);
    if (p.activity == Activity::Listening || (p.phase & 2)) pen.rect(kCx - 8, 1 + b, 16, 16, tip, 8, LV_OPA_30);
    pen.rect(kCx - 5, 4 + b, 10, 10, tip, 5);
    const uint32_t ear = p.activity == Activity::Listening ? c.tear : c.edge;
    pen.rect(7, 42 + b, 12, 26, ear, 5);
    pen.rect(105, 42 + b, 12, 26, ear, 5);

    pen.panel(14, 18 + b, 96, 74, c.shell, c.edge, 24, 2);
    pen.rect(24, 28 + b, 76, 54, c.screen, 17);

    if (look.blush) {
        const lv_opa_t opa = look.blush > 1 ? LV_OPA_90 : LV_OPA_50;
        pen.rect(28, 67 + b, 12, 6, c.blush, 3, opa);
        pen.rect(84, 67 + b, 12, 6, c.blush, 3, opa);
    }
    draw_eyes(pen, p, look, b, c);
    draw_mouth(pen, p, look, b, c);
    draw_extras(pen, p, b, c);
}

void draw_mini(lv_layer_t *layer, const lv_area_t &area, const Pose &p, const Palette &c, uint32_t frame) {
    Pen pen{layer, area.x1, area.y1};
    const Look look = look_for(p);
    pen.panel(0, 0, kMiniW, kMiniH, c.screen, frame, 7, 2);
    const int gx = p.gaze_x / 2, gy = p.gaze_y / 3;
    const int lx = 9 + gx, rx = 17 + gx, ey = 8 + gy;
    switch (look.eyes) {
    case Eyes::Happy:
        pen.arc(lx, ey + 3, 3, 2, 200, 340, c.eye);
        pen.arc(rx, ey + 3, 3, 2, 200, 340, c.eye);
        break;
    case Eyes::Closed:
    case Eyes::Sleepy:
        pen.rect(lx - 2, ey + 1, 5, 2, c.eye, 1);
        pen.rect(rx - 2, ey + 1, 5, 2, c.eye, 1);
        break;
    case Eyes::Heart:
        pen.rect(lx - 2, ey - 2, 5, 5, c.blush, 2);
        pen.rect(rx - 2, ey - 2, 5, 5, c.blush, 2);
        break;
    case Eyes::Shades:
        pen.rect(lx - 3, ey - 2, rx - lx + 7, 4, c.eye, 1);
        break;
    case Eyes::Wink:
        pen.rect(lx - 1, ey - 3, 3, 6, c.eye, 1);
        pen.arc(rx, ey + 3, 3, 2, 200, 340, c.eye);
        break;
    default: {
        const int h = clamp(6 * p.open / 100, 1, 6) + (look.eyes == Eyes::Round ? 1 : 0);
        const int w = look.eyes == Eyes::Round ? 4 : 3;
        pen.rect(lx - w / 2, ey - h / 2, w, h, c.eye, 1);
        pen.rect(rx - w / 2, ey - h / 2, w, h, c.eye, 1);
        break;
    }
    }
    const int mx = 13 + gx;
    if (look.mouth == Mouth::Talk || look.mouth == Mouth::Laugh) {
        const int h = clamp(p.mouth / 3 + 1, 1, 3);
        pen.rect(mx - 2, 14 - h / 2, 5, h, c.eye, 1);
    } else if (look.mouth == Mouth::O) {
        pen.rect(mx - 1, 13, 3, 3, c.eye, 1);
    } else if (look.mouth == Mouth::Frown) {
        pen.rect(mx - 2, 14, 5, 1, c.eye);
        pen.rect(mx - 3, 15, 1, 1, c.eye);
        pen.rect(mx + 3, 15, 1, 1, c.eye);
    } else {
        pen.rect(mx - 2, 14, 5, 1, c.eye);
        if (look.mouth == Mouth::Smile || look.mouth == Mouth::Grin) {
            pen.rect(mx - 3, 13, 1, 1, c.eye);
            pen.rect(mx + 3, 13, 1, 1, c.eye);
        }
    }
}

}  // namespace xiaozhi::face
