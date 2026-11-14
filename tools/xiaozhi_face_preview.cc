// Host preview of the Xiaozhi companion face: renders every activity and mood
// with the real LVGL software renderer into a PPM contact sheet per theme.
// Build and run with tools/xiaozhi_face_preview.sh (WSL / Linux).
#include "lvgl.h"
#include "apps/xiaozhi/xiaozhi_face.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace xiaozhi::face;

namespace {

constexpr int kCols = 6, kCellW = 132, kCellH = 162;
constexpr int kRows = 4, kW = kCols * kCellW, kH = kRows * kCellH;
uint16_t s_fb[kW * kH];
uint32_t s_ms;

struct Cell { Pose pose; const Palette *palette; uint32_t frame; bool mini; };

void flush(lv_display_t *disp, const lv_area_t *a, uint8_t *px) {
    const auto *src = reinterpret_cast<const uint16_t *>(px);
    const int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; ++y)
        std::memcpy(&s_fb[y * kW + a->x1], &src[(y - a->y1) * w], w * 2);
    lv_display_flush_ready(disp);
}

void draw(lv_event_t *e) {
    auto *cell = static_cast<Cell *>(lv_event_get_user_data(e));
    lv_area_t a;
    lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(e)), &a);
    if (cell->mini) draw_mini(lv_event_get_layer(e), a, cell->pose, *cell->palette, cell->frame);
    else draw_big(lv_event_get_layer(e), a, cell->pose, *cell->palette);
}

struct Theme { const char *name; uint32_t bg, text; Palette palette; };

}  // namespace

int main(int argc, char **argv) {
    const char *out_dir = argc > 1 ? argv[1] : ".";
    static const Theme kThemes[] = {
        {"travel", 0x181B1C, 0xEEE7D6, {0x202526, 0x59605D, 0x181B1C, 0xDDB36C, 0xFFFFFF, 0xF09B87, 0x8FC7EF, 0xDDB36C}},
        {"neon", 0x0A0B10, 0xF4F4F6, {0x17151F, 0x596373, 0x0A0B10, 0x38E8E1, 0xFFFFFF, 0xFF78CA, 0x8FC7EF, 0xFFD45A}},
        {"playful", 0xF5F7F6, 0x252D30, {0xE8EFEC, 0x7B8885, 0x252D30, 0xA5315B, 0xFFFFFF, 0xAE302E, 0x28648B, 0x805710}},
        {"classic", 0x03120B, 0xA8EDBB, {0x091E14, 0x37694B, 0x03120B, 0x8CF5A6, 0xA8EDBB, 0x8CF5A6, 0xA8EDBB, 0xF2CA6D}},
    };
    struct Sample { const char *label; Pose pose; };
    const Pose idle{Activity::Idle, Mood::Neutral, 0, 0, 0, 100, 0, 0};
    auto mood = [&](Mood m) { Pose p = idle; p.mood = m; return p; };
    auto act = [&](Activity a, uint8_t phase, int8_t gx = 0, int8_t gy = 0) {
        Pose p = idle; p.activity = a; p.phase = phase; p.gaze_x = gx; p.gaze_y = gy; return p;
    };
    Pose blink = idle; blink.open = 10;
    Pose glance = idle; glance.gaze_x = -5; glance.bob = 2;
    Pose talk = act(Activity::Speaking, 0); talk.mouth = 10;
    Pose talk_happy = talk; talk_happy.mood = Mood::Happy;
    const Sample samples[] = {
        {"idle", idle}, {"blink", blink}, {"glance", glance},
        {"listening", act(Activity::Listening, 2)}, {"thinking", act(Activity::Thinking, 12, 5, -4)},
        {"speaking", talk}, {"talk happy", talk_happy}, {"connecting", act(Activity::Connecting, 2, 3)},
        {"offline", act(Activity::Offline, 6, 0, 2)}, {"error", act(Activity::Error, 4, 0, 1)},
        {"happy", mood(Mood::Happy)}, {"laugh", mood(Mood::Laugh)}, {"sad", mood(Mood::Sad)},
        {"cry", [&] { Pose p = mood(Mood::Cry); p.phase = 3; return p; }()},
        {"angry", mood(Mood::Angry)}, {"surprised", mood(Mood::Surprised)},
        {"love", mood(Mood::Love)}, {"wink", mood(Mood::Wink)}, {"cool", mood(Mood::Cool)},
        {"sleepy", mood(Mood::Sleepy)}, {"confused", mood(Mood::Confused)}, {"shy", [&] { Pose p = mood(Mood::Shy); p.gaze_y = 3; return p; }()},
        {"voice soft", [&] { Pose p = talk; for (int i = 0; i < kWaveBars; ++i) p.bars[i] = 5 + (i * 7) % 28; return p; }()},
        {"voice loud", [&] { Pose p = talk; const uint8_t bars[kWaveBars] = {12, 28, 54, 82, 60, 36, 72, 100, 84, 46, 68, 38, 18};
            std::memcpy(p.bars, bars, sizeof(bars)); return p; }()},
    };

    lv_init();
    lv_tick_set_cb([] { return s_ms += 50; });
    lv_display_t *disp = lv_display_create(kW, kH);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static std::vector<uint8_t> buf(kW * kH * 2);
    lv_display_set_buffers(disp, buf.data(), nullptr, buf.size(), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush);

    static std::vector<Cell> cells(sizeof(samples) / sizeof(samples[0]) * 2);
    for (const Theme &theme : kThemes) {
        lv_obj_t *scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(theme.bg), 0);
        int i = 0;
        for (const Sample &s : samples) {
            const int x = (i % kCols) * kCellW + 4, y = (i / kCols) * kCellH + 4;
            for (int mini = 0; mini < 2; ++mini) {
                Cell &cell = cells[i * 2 + mini];
                cell = {s.pose, &theme.palette, theme.palette.eye, mini == 1};
                lv_obj_t *o = lv_obj_create(scr);
                lv_obj_remove_style_all(o);
                lv_obj_set_pos(o, mini ? x + 2 : x, mini ? y + kBigH + 6 : y);
                lv_obj_set_size(o, mini ? kMiniW : kBigW, mini ? kMiniH : kBigH);
                lv_obj_add_event_cb(o, draw, LV_EVENT_DRAW_MAIN, &cell);
            }
            lv_obj_t *label = lv_label_create(scr);
            lv_label_set_text(label, s.label);
            lv_obj_set_style_text_color(label, lv_color_hex(theme.text), 0);
            lv_obj_set_pos(label, x + kMiniW + 10, y + kBigH + 8);
            ++i;
        }
        lv_screen_load(scr);
        lv_refr_now(disp);

        char path[512];
        std::snprintf(path, sizeof(path), "%s/face-%s.ppm", out_dir, theme.name);
        FILE *f = std::fopen(path, "wb");
        if (!f) return 1;
        std::fprintf(f, "P6 %d %d 255\n", kW, kH);
        for (uint16_t px : s_fb) {
            const uint8_t rgb[3] = {static_cast<uint8_t>((px >> 11) << 3), static_cast<uint8_t>(((px >> 5) & 63) << 2),
                                    static_cast<uint8_t>((px & 31) << 3)};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
        std::printf("wrote %s\n", path);
        // Optional animation proof using the same production geometry and
        // envelope. Input here is a synthetic voice envelope, not device audio.
        if (argc > 2 && std::strcmp(argv[2], "wave") == 0) {
            Waveform waveform;
            const uint8_t levels[] = {0, 0, 0, 12, 30, 65, 92, 52, 20, 8, 28, 70, 100, 62, 18, 0,
                                      0, 14, 45, 85, 55, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            for (unsigned frame = 0; frame < sizeof(levels); ++frame) {
                waveform.step(levels[frame]);
                Pose &p = cells[0].pose;
                p = talk;
                p.mouth = levels[frame] ? 3 + levels[frame] / 12 : 0;
                std::memcpy(p.bars, waveform.bars, sizeof(p.bars));
                lv_obj_invalidate(scr);
                lv_refr_now(disp);
                std::snprintf(path, sizeof(path), "%s/wave-%s-%02u.ppm", out_dir, theme.name, frame);
                FILE *wave = std::fopen(path, "wb");
                if (!wave) return 1;
                std::fprintf(wave, "P6 %d %d 255\n", kBigW, kBigH);
                for (int y = 4; y < 4 + kBigH; ++y) for (int x = 4; x < 4 + kBigW; ++x) {
                    const uint16_t px = s_fb[y * kW + x];
                    const uint8_t rgb[3] = {static_cast<uint8_t>((px >> 11) << 3),
                        static_cast<uint8_t>(((px >> 5) & 63) << 2), static_cast<uint8_t>((px & 31) << 3)};
                    std::fwrite(rgb, 1, 3, wave);
                }
                std::fclose(wave);
            }
        }
        lv_obj_delete(scr);
    }
    return 0;
}
