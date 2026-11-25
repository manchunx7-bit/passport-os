// 首页(个人主页)应用。
//
// 小内存约束下的三条硬规矩(改动时请保持):
//   1. 头像索引静态存放；显示缓冲仅在首页分配，离开首页即释放;
//   2. 文件 IO(读头像)与网络 IO(拉天气)一律在**不持 LVGL 锁**时完成;
//   3. stop() 必须删屏,与 Launcher 的 launcher_ui_hide() 同理,C3 内部堆没有余量
//      同时容纳两个应用的界面。
//
// 2026-09-13 视觉精致化(全部为"对象拼画 + 低频动画",无 canvas/图片资源):
//   - 昼夜主题:任务里每小时核对,19 点~次日 6 点切夜景(ui_pixel 主题系统);
//   - 天气卡左侧画像素天气图标(晴/少云/阴/雾/雨/雪/雷),拉取中显示四点 spinner;
//   - 无头像空态换全机吉祥物(会眨眼的像素小电视),替代灰色剪影;
//   - 底部白字加墨色错位硬阴影,压在草地上也清晰。
//
// 2026-09-13 晚 · 首页版式重构(上中下三层,用户定稿):
//   顶部:像素块大时钟(5×7 字模拼块,真·像素粗体,直接画在天空上)+ 日期行;
//   中部:组合卡 = 左头像(橙框) + 右侧 天气图标/短语 + 城市 + 温度 + 湿度·风级一行
//        (原两张统计卡降维合并进这一行);
//   底部:名牌卡 = 姓名(20px 标题字) + 个性签名,浮在天空上,草地只留按键提示。
//
// 2026-09-13 深夜 · 绿磷光锁屏改版(参照用户提供的复古终端风参考图):
//   整页黑底 + 磷光绿单色,三列布局:
//   左卡(104×202):人像区(96×140,头像经亮度阈值转 A8 单色) + 姓名 + 签名;
//   右列(120 宽):点阵时钟卡(5×7 字模,每格画 4×4 光点) / 日期+城市+Wi-Fi 卡 /
//                天气卡(单色图标 + 温度 + 短语 + 分隔线 + 风向·风级);
//   底部:240×108 A8 艺术底图全宽出血(tools/gen_band_art.py 生成,内置两张,
//        长按上键切换),电池悬浮在底图右上,按键提示压在底图下缘黑条上。
//   美术显示走 LVGL A8 快速路径(alpha 蒙版 + image_recolor 纯色),已核对
//   lv_draw_sw_img.c:233 与 lv_draw_sw_blend_to_rgb565.c。
#include "apps/home/app_home.h"

#include "apps/home/band_art.h"
#include "apps/home/home_clock.h"
#include "apps/home/home_profile.h"
#include "apps/home/home_upload.h"
#include "apps/home/home_weather.h"
#include "apps/voice/time_sync.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "fonts/app_fonts.h"   // buddy_font_16:首页中文动态文本全靠它
#include "ui_pixel.h"
#include "ui/pixel_scene.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

extern "C" {
#include "bsp_battery.h"
}

#include <algorithm>
#include <ctime>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr char kTag[] = "app_home";

// A8 artwork encodes highlights: keep a dark home scene even in the light theme.
#define kBg (ui_theme_color_on_dark(UI_COLOR_BG))
#define kPhos (ui_theme_color_on_dark(UI_COLOR_TEXT))
#define kPhosDim (ui_theme_color_on_dark(UI_COLOR_TEXT_DIM))
#define kPhosBorder (ui_theme_color_on_dark(UI_COLOR_BORDER))
#define kClock (ui_theme_get() == UI_THEME_NEON || ui_theme_is_studio() ? UI_ACCENT : ui_theme_get() == UI_THEME_PLAYFUL ? ui_theme_color_on_dark(UI_COLOR_SECONDARY) : kPhos)
#define kBand (ui_theme_get() == UI_THEME_NEON || ui_theme_is_studio() ? UI_SECONDARY : ui_theme_get() == UI_THEME_PLAYFUL ? ui_theme_color_on_dark(UI_COLOR_SUCCESS) : UI_AMBER)
#define kTemperature (ui_theme_is_multicolor() ? ui_theme_color_on_dark(UI_COLOR_TERTIARY) : kPhos)
#define kWeatherAccent (ui_theme_color_on_dark(ui_theme_get() == UI_THEME_NEON || ui_theme_is_studio() ? UI_COLOR_SUCCESS : UI_COLOR_ACCENT))

// 兼容既有更新函数的语义色:磷光风格里"强调/在线"都是亮绿,弱化都是暗绿。
#define kText (kPhos)
#define kSub (kPhosDim)
#define kMuted (kPhosDim)
#define kAccent (kPhos)
#define kOnline (kPhos)
#define kOffline (kPhosDim)

// 版式栅格:所有元素都挂在这几个常量上,改一个边距整屏跟着走,不再散落硬编码。
constexpr int kMargin = 12;
constexpr int kContentW = 240 - kMargin * 2;  // 216
constexpr int kInfoLeft = 118; // Visible left edge of time, date and location pin.
constexpr int kClockSlotOffset[5] = {0, 27, 55, 62, 89};

constexpr int64_t kWeatherFirstDelayMs = 15000;
// 亮屏时天气每 15 分钟自动刷新(Open-Meteo 当前值本身按 15 分钟更新);
// 空气质量模型更新慢,保持 30 分钟,不增加 TLS 请求次数。
constexpr int64_t kWeatherPeriodMs = 900000;
constexpr int64_t kAirPeriodMs = 1800000;
// 失败(信号差/热点刚连上)后先 30 秒快速重试两次,仍失败再每 2 分钟一次,
// 避免亮屏的一分钟内一直显示"暂无",也不在断网时频繁握手。
constexpr int64_t kWeatherQuickRetryMs = 30000;
constexpr int kWeatherQuickRetries = 2;
constexpr int64_t kWeatherRetryMs = 120000;
// 超过 2 小时的天气在风级行改为显示"几小时前更新",户外断网时不把旧数据当实时。
constexpr int64_t kWeatherStaleSec = 7200;
constexpr int64_t kUploadTimeoutMs = 600000;

// Only the Home worker owns this state. Keep deadlines across app visits so a
// fresh sample does not cause another pair of TLS requests on every return.
struct WeatherSchedule {
    static constexpr unsigned Weather = 1, Air = 2, Both = Weather | Air;
    int64_t weather_due_ms = -1, air_due_ms = -1;
    int weather_failures = 0;
    char lat[HOME_COORD_MAX]{}, lon[HOME_COORD_MAX]{};

    unsigned due(int64_t now, int64_t entered, bool force, const home_profile_t &profile) const {
        if (!profile.lat[0] || !profile.lon[0]) return 0;
        if (force || (lat[0] && (std::strcmp(lat, profile.lat) || std::strcmp(lon, profile.lon)))) return Both;
        if (weather_due_ms < 0 && air_due_ms < 0) return now - entered >= kWeatherFirstDelayMs ? Both : 0;
        return (weather_due_ms <= now ? Weather : 0u) | (air_due_ms <= now ? Air : 0u);
    }

    void begin(unsigned requests, int64_t now, const home_profile_t &profile) {
        std::snprintf(lat, sizeof(lat), "%s", profile.lat);
        std::snprintf(lon, sizeof(lon), "%s", profile.lon);
        // An interrupted request stays due when the next Home worker starts.
        if (requests & Weather) weather_due_ms = now;
        if (requests & Air) air_due_ms = now;
    }

    void finish(unsigned request, int64_t now, esp_err_t result) {
        if (request == Air) {
            air_due_ms = now + kAirPeriodMs;
            return;
        }
        if (result == ESP_OK) {
            weather_failures = 0;
            weather_due_ms = now + kWeatherPeriodMs;
            return;
        }
        ++weather_failures;
        weather_due_ms = now + (weather_failures <= kWeatherQuickRetries
                                ? kWeatherQuickRetryMs : kWeatherRetryMs);
    }
};
WeatherSchedule s_weather_schedule;

// Packed two-bit tone indices; the theme supplies colors at display time.
// Home owns this buffer only while its screen/worker are alive.
uint8_t *s_portrait_packed = nullptr;

esp_err_t read_portrait(bool present) {
    if (!present) return ESP_FAIL;
    if (!s_portrait_packed)
        s_portrait_packed = static_cast<uint8_t *>(malloc(HOME_AVATAR_BYTES));
    if (!s_portrait_packed) return ESP_ERR_NO_MEM;
    return home_profile_read_avatar(s_portrait_packed, HOME_AVATAR_BYTES);
}
// 显示用 A8 展开:LVGL 软渲染 A8 快速路径(alpha 蒙版 + recolor 磷光绿纯色)。
// 2026-09-13 改为堆分配(home_start 申请 / home_stop 归还):14.4KB 常驻 .bss
// 会直接从堆里扣掉,而小智等应用恰恰在这块内存上跑 Opus 编码器 —— 之前
// largest 只剩 22KB,编码器(需约 25KB 连续块)必然 open 失败。首页与其它
// 应用互斥运行,切走时归还,谁在用谁付钱。
uint8_t *s_portrait_pixels = nullptr;
lv_image_dsc_t s_portrait_dsc;

std::atomic<bool> s_home_active{false};
// 界面存活开关:home_task 每轮持锁刷新前检查,stop() 删屏前置 false。
// 解决"stop 超时后任务仍活着却去写已删除 label"这一类的 use-after-free。
std::atomic<bool> s_ui_alive{false};
std::atomic<bool> s_force_weather{false};
int64_t s_manual_weather_ms = -30000;
TaskHandle_t s_task_handle = nullptr;
std::atomic<bool> s_worker_running{false};
uint8_t s_detail_mode = 0;

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_avatar_img = nullptr;      // Indexed portrait with a transparent background.
lv_obj_t *s_portrait_ph = nullptr;     // 无头像占位文字
lv_obj_t *s_lbl_date = nullptr;
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};
ui_pixel_battery_t s_batt = {nullptr, nullptr};
lv_obj_t *s_band_img = nullptr;        // 底部艺术底图
bool s_share_visible = false;
uint8_t s_share_index = 0;

// ---- 像素点阵时钟:4 个数字槽 + 冒号槽(槽 2),5×7 字模,每格一个 4×4 光点 ----
lv_obj_t *s_clk_slots[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
lv_obj_t *s_clk_colon[2] = {nullptr, nullptr};   // 冒号两点,每秒呼吸
char s_clk_slot_char[5] = {0, 0, 0, 0, 0};       // 各槽当前字符,变化才重绘
bool s_clk_colon_on = false;

lv_obj_t *s_lbl_name = nullptr;
lv_obj_t *s_lbl_sig = nullptr;
lv_obj_t *s_wicon_zone = nullptr;    // Weather glyph: 22×22, drawn without child objects.
lv_obj_t *s_lbl_city = nullptr;
lv_obj_t *s_lbl_wtext = nullptr;
lv_obj_t *s_lbl_temp = nullptr;
lv_obj_t *s_lbl_humidity = nullptr;  // 湿度 (网格第 1 行左)
lv_obj_t *s_lbl_wind = nullptr;      // 风向 (网格第 1 行右)
lv_obj_t *s_lbl_feels = nullptr;     // 体感 (网格第 2 行左)
lv_obj_t *s_lbl_stat = nullptr;      // 状态 (网格第 2 行右)
lv_obj_t *s_lbl_footer = nullptr;

// 底部艺术底图:0=城市夜景 1=山湖月色(均内置 flash);将来可扩展用户上传槽。
uint8_t s_band_index = 0;
lv_image_dsc_t s_band_dsc[2];

// Weather artwork is drawn in the existing zone; no retained blocks or bitmap.
uint16_t s_wicon_code = 0;
bool s_wicon_visible = false;
uint8_t s_spin_phase = 0;
bool s_wicon_spinning = false;
bool s_weather_fetching = false;

void expand_portrait(void) {
    if (!s_portrait_pixels || !s_portrait_packed) return;
    lv_image_cache_drop(&s_portrait_dsc);
    if (s_portrait_packed[4] == 0) {
        // Preserve LVGL's original RGB565 alpha blending, including its rounding.
        for (size_t pixel = 0; pixel < HOME_PORTRAIT_PIXELS; ++pixel)
            s_portrait_pixels[pixel] = home_portrait_index(s_portrait_packed, pixel) * 85;
        return;
    }
    memset(s_portrait_pixels, 0, HOME_PORTRAIT_I4_BYTES);
    auto *palette = reinterpret_cast<lv_color32_t *>(s_portrait_pixels);
    for (uint8_t i = 1; i <= 4; ++i)
        palette[i] = lv_color_to_32(lv_color_hex(ui_theme_portrait_subject_color(i - 1)), LV_OPA_COVER);
    for (size_t pixel = 0; pixel < HOME_PORTRAIT_PIXELS; pixel += 2)
        s_portrait_pixels[64 + pixel / 2] = (home_portrait_index(s_portrait_packed, pixel) << 4) |
            home_portrait_index(s_portrait_packed, pixel + 1);
}

// 无头像时显示占位文字(磷光风格不用彩色吉祥物)。
void apply_avatar(bool have_image) {
    have_image = have_image && s_portrait_packed;
    const bool legacy = have_image && s_portrait_packed[4] == 0;
    const size_t bytes = legacy ? HOME_PORTRAIT_PIXELS : HOME_PORTRAIT_I4_BYTES;
    if (have_image && s_portrait_pixels && s_portrait_dsc.data_size != bytes) {
        lv_image_set_src(s_avatar_img, nullptr);
        lv_image_cache_drop(&s_portrait_dsc);
        free(s_portrait_pixels);
        s_portrait_pixels = nullptr;
    }
    if (have_image && !s_portrait_pixels) {
        s_portrait_pixels = static_cast<uint8_t *>(malloc(bytes));
        s_portrait_dsc.data = s_portrait_pixels;
        if (!s_portrait_pixels) ESP_LOGW(kTag, "portrait buffer unavailable; using placeholder");
    }
    if (have_image && s_portrait_pixels != nullptr) {
        s_portrait_dsc.header.cf = legacy ? LV_COLOR_FORMAT_A8 : LV_COLOR_FORMAT_I4;
        s_portrait_dsc.header.stride = legacy ? HOME_PORTRAIT_W : HOME_PORTRAIT_W / 2;
        s_portrait_dsc.data_size = bytes;
        lv_obj_set_style_image_recolor(s_avatar_img, lv_color_hex(ui_theme_portrait_color(3)), 0);
        expand_portrait();
        lv_image_set_src(s_avatar_img, &s_portrait_dsc);
        lv_obj_clear_flag(s_avatar_img, LV_OBJ_FLAG_HIDDEN);
        if (s_portrait_ph) lv_obj_add_flag(s_portrait_ph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(s_avatar_img, nullptr);
        lv_obj_add_flag(s_avatar_img, LV_OBJ_FLAG_HIDDEN);
        if (s_portrait_ph) lv_obj_clear_flag(s_portrait_ph, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- 天气像素图标:WMO 码 → 方块拼画,与整机"积木"图标语言同源 ----
void update_weather_labels(const home_profile_t *prof);  // 定义在本段之后

lv_obj_t *px_container(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

lv_obj_t *px_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

// Stepped contours share a one-pixel edge and two-pixel accents at native size.
void wicon_cloud(pixel_scene::Ink &ink, int x, int y) {
    const uint32_t edge = kPhos;
    ink.box(x + 8, y, 5, 1, edge);
    ink.box(x + 6, y + 1, 2, 2, edge);
    ink.box(x + 13, y + 1, 2, 2, edge);
    ink.box(x + 3, y + 3, 4, 1, edge);
    ink.box(x + 15, y + 3, 2, 1, edge);
    ink.box(x + 1, y + 4, 2, 1, edge);
    ink.box(x + 17, y + 4, 2, 1, edge);
    ink.box(x, y + 5, 1, 4, edge);
    ink.box(x + 19, y + 5, 1, 4, edge);
    ink.box(x + 1, y + 9, 2, 1, edge);
    ink.box(x + 17, y + 9, 2, 1, edge);
    ink.box(x + 3, y + 10, 14, 1, edge);
    ink.box(x + 4, y + 8, 12, 1, kPhosDim);
}

void draw_weather_icon_event(lv_event_t *event) {
    if (!s_wicon_visible) return;
    pixel_scene::Ink ink(event);
    if (s_wicon_spinning) {
        static constexpr int dots[4][2] = {{10, 3}, {16, 9}, {10, 15}, {4, 9}};
        for (int i = 0; i < 4; ++i)
            ink.box(dots[i][0], dots[i][1], 3, 3, i == s_spin_phase ? kPhos : kPhosDim);
        return;
    }
    const uint32_t sun = ui_theme_is_multicolor()
        ? ui_theme_color_on_dark(UI_COLOR_WARNING) : kPhos;
    const uint32_t water = ui_theme_is_multicolor()
        ? ui_theme_color_on_dark(UI_COLOR_SECONDARY) : kPhos;
    const uint16_t code = s_wicon_code;
    if (code == 0) {
        // Open solar disc with eight short, balanced rays.
        ink.box(8, 6, 6, 2, sun); ink.box(8, 14, 6, 2, sun);
        ink.box(6, 8, 2, 6, sun); ink.box(14, 8, 2, 6, sun);
        ink.box(10, 1, 2, 3, sun); ink.box(10, 18, 2, 3, sun);
        ink.box(1, 10, 3, 2, sun); ink.box(18, 10, 3, 2, sun);
        ink.box(3, 3, 2, 2, sun); ink.box(17, 3, 2, 2, sun);
        ink.box(3, 17, 2, 2, sun); ink.box(17, 17, 2, 2, sun);
        return;
    }
    if (code == 1 || code == 2) {
        // The sun peeks above the cloud; keep the foreground outline clear.
        ink.box(14, 3, 4, 1, sun); ink.box(13, 4, 1, 2, sun);
        ink.box(18, 4, 1, 3, sun); ink.box(16, 0, 1, 2, sun);
        ink.box(20, 2, 2, 1, sun); ink.box(20, 6, 2, 1, sun);
        wicon_cloud(ink, 1, 8);
        return;
    }
    const bool fog = code == 45 || code == 48;
    const bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
    const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    const bool thunder = code >= 95;
    wicon_cloud(ink, 1, fog || rain || snow || thunder ? 2 : 5);
    if (fog) {
        ink.box(3, 16, 11, 1, kPhosDim); ink.box(16, 16, 3, 1, kPhosDim);
        ink.box(5, 19, 13, 1, kPhosDim);
    } else if (rain) {
        for (int x : {5, 11, 17}) {
            ink.box(x, 16, 1, 2, water); ink.box(x - 1, 18, 1, 2, water);
        }
    } else if (snow) {
        for (int x : {6, 15}) {
            ink.box(x, 16, 1, 5, water); ink.box(x - 2, 18, 5, 1, water);
        }
    } else if (thunder) {
        ink.box(11, 14, 3, 2, sun); ink.box(9, 16, 4, 2, sun);
        ink.box(11, 18, 2, 1, sun); ink.box(10, 19, 2, 2, sun);
    }
}

void wicon_clear(void) {
    s_wicon_visible = false;
    s_wicon_spinning = false;
    if (s_ui_alive.load() && s_wicon_zone) lv_obj_invalidate(s_wicon_zone);
}

void weather_icon_draw(uint16_t code) {
    s_wicon_code = code;
    s_wicon_visible = true;
    s_wicon_spinning = false;
    if (s_wicon_zone) lv_obj_invalidate(s_wicon_zone);
}

void weather_spinner_draw(void) {
    s_spin_phase = 0;
    s_wicon_visible = true;
    s_wicon_spinning = true;
    if (s_wicon_zone) lv_obj_invalidate(s_wicon_zone);
}

void weather_icon_spin_tick(void) {
    if (!s_wicon_spinning || !s_ui_alive.load() || !s_wicon_zone) return;
    s_spin_phase = (s_spin_phase + 1) & 3;
    lv_obj_invalidate(s_wicon_zone);
}

// 天气区整体刷新:文字行 + 左侧图标。调用方需已持 LVGL 锁。
void refresh_weather_ui(void) {
    if (!s_ui_alive.load() || s_screen == nullptr) return;
    home_profile_t prof;
    home_profile_get(&prof);
    update_weather_labels(&prof);
    if (prof.lat[0] == '\0') {
        wicon_clear();                       // 未设城市:只留文字提示
    } else if (s_weather_fetching) {
        weather_spinner_draw();
    } else {
        home_weather_t w;
        if (home_weather_get(&w) && w.valid) {
            weather_icon_draw(w.code);
        } else {
            wicon_clear();                   // 无数据:不画,文字会提示"获取中"
        }
    }
}

// ---- 点阵时钟 ----
// 5×7 字模(行位掩码,bit4=最左列),每个点亮格画一个 4×4 光点(格距 5),
// 呈现 LED 点阵质感,与磷光锁屏参考图一致。不依赖任何字体文件。
constexpr int kCell = 5;             // 格距 → 数字 25×35
constexpr int kDot = 4;              // 光点边长(留 1px 缝出点阵感)
constexpr int kDigitW = 5 * kCell;   // 25
constexpr int kClockH = 7 * kCell;   // 35
constexpr uint8_t kDashGlyphIdx = 10;
const uint8_t kDigitGlyphs[11][7] = {
    {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x1F, 0x01, 0x01, 0x1F, 0x10, 0x10, 0x1F},  // 2
    {0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x1F},  // 3
    {0x11, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x01},  // 4
    {0x1F, 0x10, 0x10, 0x1F, 0x01, 0x01, 0x1F},  // 5
    {0x1F, 0x10, 0x10, 0x1F, 0x11, 0x11, 0x1F},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x04, 0x04, 0x04},  // 7
    {0x1F, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x1F},  // 8
    {0x1F, 0x11, 0x11, 0x1F, 0x01, 0x01, 0x1F},  // 9
    {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00},  // '-' 占位
};

// Draw the same pixel glyph directly. A digit no longer owns 10-25 child
// objects, and a minute change no longer frees/reallocates their styles.
void draw_digit_event(lv_event_t *event) {
    auto *slot = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const char c = *static_cast<const char *>(lv_event_get_user_data(event));
    lv_area_t origin;
    lv_obj_get_coords(slot, &origin);
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_hex(kClock);
    ink.bg_opa = LV_OPA_COVER;
    int idx = kDashGlyphIdx;
    if (c >= '0' && c <= '9') idx = c - '0';
    // The panel is refreshed in 12-row bands; only queue dots inside this band.
    lv_layer_t *layer = lv_event_get_layer(event);
    const lv_area_t clip = layer->_clip_area;
    for (int r = 0; r < 7; ++r) {
        const int y = origin.y1 + r * kCell;
        if (y > clip.y2) break;
        if (y + kDot - 1 < clip.y1) continue;
        for (int col = 0; col < 5; ++col) {
            if (kDigitGlyphs[idx][r] & (0x10 >> col)) {
                const int x = origin.x1 + col * kCell;
                const lv_area_t dot = {x, y, x + kDot - 1, y + kDot - 1};
                lv_draw_rect(layer, &ink, &dot);
            }
        }
    }
}

// 数字槽重绘 + 日期 + 冒号每秒呼吸(时钟区唯一的逐秒动作,只动 opa 不重排)
void update_clock_labels() {
    char hh[3] = "--";
    char mm[3] = "--";
    char date_buf[24];
    const bool valid = home_clock_valid();
    if (valid) {
        char time_buf[8];
        home_clock_format_time(time_buf, sizeof(time_buf));
        hh[0] = time_buf[0];
        hh[1] = time_buf[1];
        mm[0] = time_buf[3];
        mm[1] = time_buf[4];
        char ymd[16];
        char week[8];
        home_clock_format_date(ymd, sizeof(ymd));
        home_clock_format_weekday(week, sizeof(week));
        std::snprintf(date_buf, sizeof(date_buf), "%s %s", ymd, week);
    } else {
        // 未校时一律显示占位符。时间只能来自 SNTP 或 PC 校时,设备自己没有可信来源。
        std::snprintf(date_buf, sizeof(date_buf), "等待校时");
    }

    // The leading '1' and unsynchronized dash have one empty pixel column.
    // Shift the whole clock as a group so its visible ink starts at kInfoLeft,
    // retaining the existing spacing between digits and colon.
    const int clock_inset = (hh[0] == '1' || hh[0] == '-') ? kCell : 0;
    for (int i = 0; i < 5; ++i) {
        const int x = kInfoLeft + kClockSlotOffset[i] - clock_inset;
        if (s_clk_slots[i] && lv_obj_get_x(s_clk_slots[i]) != x)
            lv_obj_set_x(s_clk_slots[i], x);
    }
    const char digits[4] = {hh[0], hh[1], mm[0], mm[1]};
    const int slot_map[4] = {0, 1, 3, 4};   // 槽 2 是冒号
    for (int i = 0; i < 4; ++i) {
        const int slot = slot_map[i];
        if (s_clk_slots[slot] && s_clk_slot_char[slot] != digits[i]) {
            s_clk_slot_char[slot] = digits[i];
            lv_obj_invalidate(s_clk_slots[slot]);
        }
    }
    ui_pixel_label_set_text(s_lbl_date, date_buf);
    lv_font_glyph_dsc_t first_glyph{};
    const uint32_t first_codepoint = valid ? static_cast<uint8_t>(date_buf[0]) : 0x7b49; // 等
    const int date_inset = lv_font_get_glyph_dsc(&buddy_font_16, &first_glyph, first_codepoint, 0)
        ? first_glyph.ofs_x : 0;
    // The subset glyph for 等 also has one transparent column inside its box.
    lv_obj_set_x(s_lbl_date, kInfoLeft - date_inset - (valid ? 0 : 1));

    s_clk_colon_on = !s_clk_colon_on;
    for (lv_obj_t *dot : s_clk_colon) {
        if (dot) {
            lv_obj_set_style_opa(dot, s_clk_colon_on ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
}

// 电量画成一个 24×12 的迷你电池:外框 + 按 SOC 伸缩的内条 + 右侧百分比数字。
// 比单纯的 "78%" 多占了两个对象,但一眼能读出剩余量,是值得的。
// 顶栏电池与 Wi-Fi 信号都用共享像素构件,与 Launcher/AI语音 顶栏一致。
void update_battery_label() {
    ui_pixel_battery_set_on_dark(&s_batt, bsp_battery_soc());

    const bool connected = WifiManager::GetInstance().IsConnected();
    int bars = 0;
    uint32_t color = kOffline;
    if (connected) {
        const int rssi = WifiManager::GetInstance().GetRssi();
        bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
        color = kOnline;
    }
    ui_pixel_wifi_bars_set(s_wifi_bars, bars, color);
}

// 天气拆三段:城市(弱) / 天气短语(次) / 温度(强调色 + 20px 数字)。
// 温度用 montserrat 而不是中文字库,是因为 "24°" 这种字形的数字比中文点阵细腻得多。
void update_weather_labels(const home_profile_t *prof) {
    if (!s_ui_alive.load() || s_lbl_city == nullptr) return;
    char pm_line[32];
    home_air_quality_label(WifiManager::GetInstance().IsConnected(), pm_line, sizeof(pm_line));
    if (s_lbl_stat) lv_label_set_text(s_lbl_stat, pm_line);
    if (prof->lat[0] == '\0') {
        lv_label_set_text(s_lbl_city, "未设城市");
        lv_label_set_text(s_lbl_wtext, "");
        lv_label_set_text(s_lbl_temp, "--");
        if (s_lbl_humidity) lv_label_set_text(s_lbl_humidity, "湿度 --");
        if (s_lbl_feels) lv_label_set_text(s_lbl_feels, "体感 --");
        return;
    }
    home_weather_t w;
    const bool ok = home_weather_get(&w);
    lv_label_set_text(s_lbl_city, prof->city);
    if (ok && w.valid) {
        lv_label_set_text(s_lbl_wtext, home_weather_code_text(w.code));
        char t[12];
        std::snprintf(t, sizeof(t), "%d°", static_cast<int>(w.temp_c));
        lv_label_set_text(s_lbl_temp, t);

        char h[20];
        std::snprintf(h, sizeof(h), "湿度 %u%%", (unsigned)w.humidity);
        if (s_lbl_humidity) lv_label_set_text(s_lbl_humidity, h);

        int feels = static_cast<int>(w.temp_c);
        if (w.temp_c >= 25 && w.humidity > 60) {
            feels += (w.humidity - 60) / 12;
        } else if (w.temp_c < 15 && w.wind_kmh > 15) {
            feels -= (w.wind_kmh - 15) / 10;
        }
        char f[20];
        std::snprintf(f, sizeof(f), "体感 %d°", feels);
        if (s_lbl_feels) lv_label_set_text(s_lbl_feels, f);

    } else {
        lv_label_set_text(s_lbl_wtext, s_weather_fetching ? "更新" : "暂无");
        lv_label_set_text(s_lbl_temp, "--");
        if (s_lbl_humidity) lv_label_set_text(s_lbl_humidity, "湿度 --");
        if (s_lbl_feels) lv_label_set_text(s_lbl_feels, "体感 --");
    }
}

// 天气卡风级行:风向 + 蒲福风级 (例如:北风 3级)。
void update_wind_line() {
    if (!s_ui_alive.load() || s_lbl_wind == nullptr) return;
    if (s_lbl_stat) {
        char line[32];
        home_air_quality_label(WifiManager::GetInstance().IsConnected(), line, sizeof(line));
        ui_pixel_label_set_text(s_lbl_stat, line);
    }
    home_weather_t w;
    if (home_weather_get(&w) && w.valid) {
        char line[40];
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        const bool unknown_age = w.updated_at == 0 || !home_clock_valid();
        const bool stale = unknown_age
            ? !WifiManager::GetInstance().IsConnected()
            : now - static_cast<int64_t>(w.updated_at) >= kWeatherStaleSec;
        if (stale) {
            char age[24];
            home_weather_age_text(w.updated_at, unknown_age ? 0 : now, age, sizeof(age));
            if (unknown_age) std::snprintf(line, sizeof(line), "%s", age);
            else std::snprintf(line, sizeof(line), "%s更新", age);
        } else {
            std::snprintf(line, sizeof(line), "%s %d级",
                          home_weather_wind_dir_text(w.wind_dir),
                          home_weather_beaufort(w.wind_kmh));
        }
        ui_pixel_label_set_text(s_lbl_wind, line);
    } else {
        ui_pixel_label_set_text(s_lbl_wind, "风向 --");
    }
}

// 底部一行在"操作提示 / 上传提示 / 详情"之间轮换(下键切换),
// 上传模式优先级最高 —— 那时用户正盯着手机扫码,必须立刻看到 URL。
// 按键提示用暗色退后(与其他页面一致);IP/PM2.5/上传地址等信息行保持亮色。
void footer_set(const char *text, const lv_font_t *font, uint32_t color) {
    if (lv_obj_get_style_text_font(s_lbl_footer, LV_PART_MAIN) != font)
        lv_obj_set_style_text_font(s_lbl_footer, font, 0);
    ui_pixel_text_color(s_lbl_footer, color);
    ui_pixel_label_set_text(s_lbl_footer, text);

}

void update_footer_label() {
    if (home_upload_active()) {
        footer_set(home_upload_url(), &lv_font_montserrat_14, kPhos);
        return;
    }

    if (s_detail_mode == 1) {
        footer_set("长按上换图 · 长按下设置", &buddy_font_16, kPhosDim);
        return;
    }
    if (s_detail_mode == 2) {
        if (!WifiManager::GetInstance().IsConnected()) {
            footer_set("未连接无线网络", &buddy_font_16, kPhos);
            return;
        }
        home_weather_t w;
        const bool ok = home_weather_get(&w);
        const std::string ip = WifiManager::GetInstance().GetIpAddress();
        char line[48];
        char age[24] = "暂无天气";
        if (ok && w.valid) {
            home_weather_age_text(w.updated_at,
                                  home_clock_valid() ? static_cast<int64_t>(std::time(nullptr)) : 0,
                                  age, sizeof(age));
        }
        std::snprintf(line, sizeof(line), "%s · 确定刷新", age);
        footer_set(line, &buddy_font_16, kPhos);
        return;
    }
    if (s_detail_mode == 3) {
        home_air_quality_t air;
        if (WifiManager::GetInstance().IsConnected() && home_air_quality_get(&air)) {
            char line[48];
            std::snprintf(line, sizeof(line), "PM2.5 %u.%u ug/m3",
                          (unsigned)(air.pm25_x10 / 10), (unsigned)(air.pm25_x10 % 10));
            footer_set(line, &lv_font_montserrat_14, kPhos);
        } else {
            footer_set("PM2.5 暂无数据", &buddy_font_16, kPhos);
        }
        return;
    }
    if (s_detail_mode == 4) {
        footer_set("Open-Meteo/CAMS 估算", &buddy_font_16, kPhos);
        return;
    }
    footer_set("上名片 · 下详情 · 确定应用", &buddy_font_16, kPhosDim);
}

void apply_profile(const home_profile_t *prof) {
    lv_label_set_text(s_lbl_name, prof->name[0] ? prof->name : "未设置姓名");
    lv_label_set_text(s_lbl_sig, prof->signature[0] ? prof->signature : "未设置签名");
    refresh_weather_ui();
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                     lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

// Passport paper details are drawn on demand without retained child objects.
void draw_passport_details(lv_event_t *event) {
    pixel_scene::Ink ink(event);
    // Open portrait and perforated seam replace retained decoration objects.
    const uint32_t surface = ui_theme_color_on_dark(UI_COLOR_SURFACE);
    const bool studio = ui_theme_is_studio();
    if (studio) {
        ink.rbox(116, 32, 118, 93, surface, 8, kPhosBorder, 1);
        ink.rbox(116, 128, 118, 108, surface, 8, kPhosBorder, 1);
    } else {
        ink.box(116, 32, 118, 93, surface);
        ink.box(116, 128, 118, 108, surface);
    }
    // 9x12 outlined map pin: stepped crown, open center and tapered tip.
    // Center y=110.5 matches the visible city glyphs (y=104..117).
    // Draw with the page, avoiding two retained decoration objects.
    const uint32_t pin = studio ? UI_SECONDARY : kPhos;
    ink.box(kInfoLeft + 2, 105, 5, 1, pin);
    ink.box(kInfoLeft + 1, 106, 1, 1, pin); ink.box(kInfoLeft + 7, 106, 1, 1, pin);
    ink.box(kInfoLeft + 0, 107, 1, 4, pin); ink.box(kInfoLeft + 8, 107, 1, 4, pin);
    ink.box(kInfoLeft + 3, 108, 3, 2, kPhosDim);
    ink.box(kInfoLeft + 1, 111, 1, 1, pin); ink.box(kInfoLeft + 7, 111, 1, 1, pin);
    ink.box(kInfoLeft + 2, 112, 1, 1, pin); ink.box(kInfoLeft + 6, 112, 1, 1, pin);
    ink.box(kInfoLeft + 3, 113, 1, 1, pin); ink.box(kInfoLeft + 5, 113, 1, 1, pin);
    ink.box(kInfoLeft + 4, 114, 1, 3, pin);
    for (int y = 34; y < 234; y += 8) ink.box(110, y, 1, 3, kPhosBorder);
    ink.box(14, 32, 14, 2, kPhosBorder); ink.box(14, 32, 2, 12, kPhosBorder);
    ink.box(88, 158, 14, 2, kPhosBorder); ink.box(100, 148, 2, 12, kPhosBorder);
    if (studio) {
        ink.rbox(14, 172, 22, 3, UI_ACCENT, 1);
        ink.rbox(40, 172, 12, 3, UI_SECONDARY, 1);
        ink.rbox(56, 172, 8, 3, UI_TERTIARY, 1);
        ink.box(68, 173, 34, 1, kPhosBorder);
    } else {
        ink.box(14, 172, 24, 2, kPhos);
        ink.box(42, 172, 60, 1, kPhosBorder);
    }
    for (int i = 0; i < 18; ++i) ink.box(14 + i * 5, 226, i % 3 ? 1 : 2, i % 4 ? 5 : 8, kPhosBorder);
    ink.box(120, 162, 110, 1, kPhosBorder);
}

void create_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    // ---- 状态栏 (y0..24):Wi-Fi 信号 + 电池,回到最顶部 ----
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    if (ui_theme_get() == UI_THEME_PLAYFUL) {
        ui_pixel_bg_color(top, kBg);
        lv_obj_set_style_border_color(top, lv_color_hex(kPhosBorder), 0);
    }
    ui_pixel_wifi_bars_create(s_screen, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    s_batt = ui_pixel_battery_create(s_screen, UI_STATUS_BATTERY_X, 6, kPhos, &lv_font_montserrat_14);

    lv_obj_t *passport = ui_pixel_label(s_screen, "PASSPORT", &lv_font_montserrat_14, kPhosDim);
    lv_obj_set_pos(passport, 42, 4);

    lv_obj_add_event_cb(s_screen, draw_passport_details, LV_EVENT_DRAW_MAIN, nullptr);

    // ---- 左侧区:人像 + 姓名 + 签名 (自由站立，通透无盒) ----
    // 人像区 96×138
    lv_obj_t *zone = px_container(s_screen, 12, 30, 96, 138);
    lv_obj_set_style_border_width(zone, 0, 0);
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);

    s_avatar_img = lv_image_create(zone);
    lv_obj_set_pos(s_avatar_img, 0, 0);
    lv_obj_set_size(s_avatar_img, HOME_PORTRAIT_W, HOME_PORTRAIT_H);
    lv_image_set_inner_align(s_avatar_img, LV_IMAGE_ALIGN_CENTER);
    // A8 alpha 图以 recolor 色实心填充(LVGL 软渲染快速路径,已核对 lv_draw_sw_img.c)
    lv_obj_set_style_image_recolor_opa(s_avatar_img, LV_OPA_TRANSP, 0);

    s_portrait_ph = make_label(zone, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_portrait_ph, 92);
    lv_obj_align(s_portrait_ph, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_portrait_ph, "头像未设置");

    s_lbl_name = make_label(s_screen, &buddy_font_16, kPhos, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_letter_space(s_lbl_name, UI_LS_BODY, 0);
    lv_obj_set_size(s_lbl_name, 90, 20);
    lv_label_set_long_mode(s_lbl_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_lbl_name, 14, 182);

    s_lbl_sig = make_label(s_screen, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_size(s_lbl_sig, 90, 20);
    lv_label_set_long_mode(s_lbl_sig, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_lbl_sig, 14, 204);
    lv_label_set_text(s_lbl_name, "未设置姓名");
    lv_label_set_text(s_lbl_sig, "未设置签名");

    // Clock sits 14px below the status bar; date and city form a compact group.
    for (int i = 0; i < 5; ++i) {
        s_clk_slots[i] = px_container(s_screen, kInfoLeft + kClockSlotOffset[i], 40,
                                      i == 2 ? 4 : kDigitW, kClockH);
        if (i != 2) lv_obj_add_event_cb(s_clk_slots[i], draw_digit_event,
                                       LV_EVENT_DRAW_MAIN, &s_clk_slot_char[i]);
    }
    s_clk_colon[0] = px_block(s_clk_slots[2], 0, 7, 4, 4, kClock);
    s_clk_colon[1] = px_block(s_clk_slots[2], 0, 21, 4, 4, kClock);

    // Keep 6px between the 35px clock and its date line.
    s_lbl_date = make_label(s_screen, &buddy_font_16, kPhos, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_lbl_date, 114);
    lv_obj_set_pos(s_lbl_date, kInfoLeft, 81);

    // Four clear pixels separate the outlined pin from the city name.
    s_lbl_city = make_label(s_screen, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_lbl_city, 131, 102);
    lv_obj_set_size(s_lbl_city, 93, 20);
    lv_label_set_long_mode(s_lbl_city, LV_LABEL_LONG_MODE_DOTS);

    // ---- 右下环境气象区 (x114..234, y124..236: 经典三行大字版式) ----
    // 行 1: 天气图标 + 大温度 + 天气状态
    s_wicon_zone = px_container(s_screen, 116, 135, 22, 22);
    lv_obj_add_event_cb(s_wicon_zone, draw_weather_icon_event, LV_EVENT_DRAW_MAIN, nullptr);
    s_lbl_temp = make_label(s_screen, &lv_font_montserrat_20, kTemperature, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_lbl_temp, 142, 134);
    lv_obj_set_size(s_lbl_temp, 42, 24);
    lv_label_set_long_mode(s_lbl_temp, LV_LABEL_LONG_MODE_CLIP);

    s_lbl_wtext = make_label(s_screen, &buddy_font_16, kWeatherAccent, LV_TEXT_ALIGN_LEFT);
    // Three full-width Chinese glyphs need 48px even without tracking.
    // Bound the height as well: DOTS on a content-height label can still wrap.
    lv_obj_set_pos(s_lbl_wtext, 186, 137);
    lv_obj_set_size(s_lbl_wtext, 48, 20);
    lv_obj_set_style_text_letter_space(s_lbl_wtext, 0, 0);
    lv_label_set_long_mode(s_lbl_wtext, LV_LABEL_LONG_MODE_DOTS);

    // 三行垂直清晰气象指标 (y=170, 192, 214), 采用 16px buddy_font_16，舒适清晰
    s_lbl_humidity = make_label(s_screen, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_lbl_humidity, 118, 170);
    lv_obj_set_size(s_lbl_humidity, 112, 20);
    lv_label_set_long_mode(s_lbl_humidity, LV_LABEL_LONG_MODE_DOTS);

    s_lbl_wind = make_label(s_screen, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_lbl_wind, 118, 192);
    lv_obj_set_size(s_lbl_wind, 112, 20);
    lv_label_set_long_mode(s_lbl_wind, LV_LABEL_LONG_MODE_DOTS);

    s_lbl_stat = make_label(s_screen, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_lbl_stat, 118, 214);
    lv_obj_set_size(s_lbl_stat, 112, 20);
    lv_label_set_long_mode(s_lbl_stat, LV_LABEL_LONG_MODE_DOTS);

    s_lbl_feels = nullptr;

    lv_label_set_text(s_lbl_temp, "--");
    lv_label_set_text(s_lbl_wtext, "");
    lv_label_set_text(s_lbl_humidity, "湿度 --");
    lv_label_set_text(s_lbl_stat, "PM2.5 --");
    ui_pixel_label_set_text(s_lbl_wind, "风向 --");

    // ---- 底部艺术底图 (y240..320):A8 alpha 图 + 磷光绿 recolor,全宽出血 ----
    s_band_img = lv_image_create(s_screen);
    lv_image_set_src(s_band_img, &s_band_dsc[s_band_index]);
    lv_obj_set_pos(s_band_img, 0, 240);
    lv_obj_set_style_image_recolor(s_band_img, lv_color_hex(kBand), 0);

    // ---- 按键提示:底黑条 + 1px 细分隔线 + 提拉 6px 安全边距 ----
    px_block(s_screen, 0, 292, 240, 1, kPhosBorder);
    lv_obj_t *strip = px_block(s_screen, 0, 293, 240, 27, kBg);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    s_lbl_footer = make_label(s_screen, &buddy_font_16, kPhosDim, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_lbl_footer, kContentW);
    lv_obj_align(s_lbl_footer, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_label_set_long_mode(s_lbl_footer, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_lbl_footer, "上名片 · 下详情 · 确定应用");
}

constexpr uint16_t kJpegMcuHeight = 16;
constexpr std::size_t kJpegWorkSize = 4096;

struct ShareJpegContext {
    FILE *fp;
    uint16_t *stripe;
    uint16_t width;
    uint16_t height;
    bool color_pending;
    bool failed;
};

std::size_t share_jpeg_input(JDEC *decoder, uint8_t *buffer, std::size_t requested) {
    auto *context = static_cast<ShareJpegContext *>(decoder->device);
    if (!context || !context->fp) return 0;
    if (buffer) {
        return std::fread(buffer, 1, requested, context->fp);
    } else {
        return std::fseek(context->fp, static_cast<long>(requested), SEEK_CUR) == 0 ? requested : 0;
    }
}

int share_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rectangle) {
    auto *context = static_cast<ShareJpegContext *>(decoder->device);
    if (!context || !bitmap || !rectangle || context->failed) return 0;

    const uint16_t block_width = rectangle->right - rectangle->left + 1;
    const uint16_t block_height = rectangle->bottom - rectangle->top + 1;
    if (rectangle->right >= context->width || rectangle->bottom >= context->height) {
        context->failed = true;
        return 0;
    }

    if (rectangle->left == 0 && context->color_pending) {
        if (bsp_display_io()) {
            esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
        }
        context->color_pending = false;
    }

    uint16_t *target = context->stripe;
    if (!target) {
        context->failed = true;
        return 0;
    }

    // TJPGD 输出格式为 BGR888
    const auto *bgr = static_cast<const uint8_t *>(bitmap);
    for (uint16_t y = 0; y < block_height; ++y) {
        uint16_t *row = target + static_cast<std::size_t>(y) * context->width;
        for (uint16_t x = 0; x < block_width; ++x) {
            const std::size_t input = (static_cast<std::size_t>(y) * block_width + x) * 3U;
            const uint16_t color = static_cast<uint16_t>(
                ((bgr[input + 2] & 0xf8U) << 8U) |
                ((bgr[input + 1] & 0xfcU) << 3U) |
                (bgr[input] >> 3U));
            row[rectangle->left + x] = __builtin_bswap16(color);
        }
    }

    if (rectangle->right + 1U == context->width) {
        const uint16_t panel_top = rectangle->top;
        const uint16_t panel_bottom = rectangle->bottom + 1U;
        if (esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, panel_top,
                                      context->width, panel_bottom, target) != ESP_OK) {
            context->failed = true;
            return 0;
        }
        context->color_pending = true;
    }
    return 1;
}

bool home_show_share_image() {
    char path[HOME_SHARE_PATH_MAX];
    if (!home_profile_share_path_at(s_share_index, path, sizeof(path))) return false;
    FILE *fp = std::fopen(path, "rb");
    if (!fp) {
        ESP_LOGW(kTag, "Cannot open share image: %s", path);
        return false;
    }

    auto *work = static_cast<uint8_t *>(std::malloc(kJpegWorkSize));
    auto *stripe = static_cast<uint16_t *>(
        std::malloc(HOME_SHARE_W * kJpegMcuHeight * sizeof(uint16_t)));
    if (!work || !stripe) {
        ESP_LOGE(kTag, "Alloc failed for share JPEG decoding");
        if (work) std::free(work);
        if (stripe) std::free(stripe);
        std::fclose(fp);
        return false;
    }

    ShareJpegContext context = {
        .fp = fp,
        .stripe = stripe,
        .width = HOME_SHARE_W,
        .height = HOME_SHARE_H,
        .color_pending = false,
        .failed = false,
    };

    JDEC decoder;
    JRESULT res = jd_prepare(&decoder, share_jpeg_input, work, kJpegWorkSize, &context);
    if (res != JDR_OK) {
        ESP_LOGW(kTag, "jd_prepare failed: %d", res);
        std::free(work);
        std::free(stripe);
        std::fclose(fp);
        return false;
    }

    if (decoder.width > HOME_SHARE_W || decoder.height > HOME_SHARE_H) {
        ESP_LOGW(kTag, "Share image dimensions (%ux%u) exceed max (%ux%u)",
                 decoder.width, decoder.height, HOME_SHARE_W, HOME_SHARE_H);
        std::free(work);
        std::free(stripe);
        std::fclose(fp);
        return false;
    }
    context.width = decoder.width;
    context.height = decoder.height;

    res = jd_decomp(&decoder, share_jpeg_output, 0);
    std::fclose(fp);

    if (context.color_pending && bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }

    std::free(work);
    std::free(stripe);

    if (res != JDR_OK || context.failed) {
        ESP_LOGW(kTag, "jd_decomp failed: %d (failed=%d)", res, context.failed);
        return false;
    }

    ESP_LOGI(kTag, "Wallet card displayed: %ux%u color JPEG index=%u free=%u largest=%u",
        decoder.width, decoder.height, s_share_index, (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

void toggle_share_view() {
    if (s_share_visible) {
        bsp_lvgl_lock(-1);
        s_share_visible = false;
        lv_display_t *disp = lv_display_get_default();
        if (disp) lv_display_enable_invalidation(disp, true);
        if (s_screen) lv_obj_invalidate(s_screen);
        bsp_lvgl_unlock();
        return;
    }

    if (!home_profile_has_share_image()) {
        ESP_LOGW(kTag, "UP: no valid wallet card file");
        // 尚未上传随身名片时保留原有的手动天气刷新能力。
        s_force_weather.store(true);
        return;
    }

    bsp_lvgl_lock(-1);
    s_share_index = 0;
    s_share_visible = true;
    lv_display_t *disp = lv_display_get_default();
    if (disp) lv_display_enable_invalidation(disp, false);
    bsp_lvgl_unlock();

    // 在锁外执行文件读取与分块直推屏幕
    if (!home_show_share_image()) {
        bsp_lvgl_lock(-1);
        s_share_visible = false;
        if (disp) lv_display_enable_invalidation(disp, true);
        if (s_screen) lv_obj_invalidate(s_screen);
        bsp_lvgl_unlock();
    }
}

// 长按上键:循环切换底部艺术底图(城市夜景 → 山湖月色;将来加入用户上传槽)。
void band_art_next(void) {
    s_band_index = (s_band_index + 1) % 2;
    if (s_band_img) {
        lv_image_set_src(s_band_img, &s_band_dsc[s_band_index]);
    }
}

void home_task(void *arg) {
    (void)arg;
    const int64_t start_ms = esp_timer_get_time() / 1000;
    int64_t upload_started_ms = 0;
    bool upload_ps_high = false; // 上传模式是否已切 BALANCED 省电档
    bool clock_on = false;
    uint32_t last_profile_ver = home_profile_version();
    home_profile_t prof;
    home_profile_get(&prof);

    while (s_home_active.load()) {
        const int64_t now_ms = esp_timer_get_time() / 1000;

        if (bsp_lvgl_lock(1000)) {
            if (s_ui_alive.load() && app_registry_screen_is_on() && !s_share_visible) {
                update_clock_labels();
                update_battery_label();
                update_wind_line();
                // 底部行每秒重算:上传模式下它会把刚拿到的 IP 补进 URL,
                // 避免"Wi-Fi 刚连上还没拿到地址"时显示成空的 http://。
                update_footer_label();
                weather_icon_spin_tick();
            }
            bsp_lvgl_unlock();
        }

        // Wi-Fi 就绪才跑 SNTP;断开即停,避免 lwIP 一直重试。
        const bool connected = WifiManager::GetInstance().IsConnected();
        if (connected && !clock_on) {
            home_clock_start();
            clock_on = true;
        } else if (!connected && clock_on) {
            home_clock_stop();
            clock_on = false;
        }

        // 资料(含头像)变化:先在锁外做文件 IO,再持锁更新 UI。
        const uint32_t profile_ver = home_profile_version();
        if (profile_ver != last_profile_ver) {
            last_profile_ver = profile_ver;
            char previous_lat[HOME_COORD_MAX], previous_lon[HOME_COORD_MAX];
            std::snprintf(previous_lat, sizeof(previous_lat), "%s", prof.lat);
            std::snprintf(previous_lon, sizeof(previous_lon), "%s", prof.lon);
            home_profile_get(&prof);
            if (std::strcmp(previous_lat, prof.lat) || std::strcmp(previous_lon, prof.lon))
                s_force_weather.store(true);
            const bool has = home_profile_has_avatar();
            esp_err_t err = ESP_FAIL;
            if (has) err = read_portrait(true);
            if (bsp_lvgl_lock(1000)) {
                if (s_ui_alive.load()) {
                    apply_profile(&prof);
                    apply_avatar(err == ESP_OK);
                }
                bsp_lvgl_unlock();
            }
        }

        // 首次进入延迟 15s;跨页面复用样本,到期自动刷新,手动刷新或换城市立即请求。
        // 熄屏期间不发请求(省电);亮屏后若已到期,下一轮循环立即刷新。
        home_profile_get(&prof);
        const unsigned requests = connected && app_registry_screen_is_on()
            ? s_weather_schedule.due(now_ms, start_ms, s_force_weather.load(), prof) : 0;
        if (requests) {
            s_force_weather.store(false);
            s_weather_schedule.begin(requests, now_ms, prof);
            // 拉取期间天气卡左侧转四点 spinner(每秒转一格),结束后按结果换图标
            s_weather_fetching = true;
            if (bsp_lvgl_lock(1000)) {
                refresh_weather_ui();
                bsp_lvgl_unlock();
            }
            // 首页平时用 MAX modem sleep(约 1 秒才收一次下行),TLS 多次往返会
            // 把建连拖到接近超时。请求期间临时切到 MIN modem,结束即恢复;
            // 应用切换会等本任务退出后再设置下一个应用的档位。
            WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::BALANCED);
            if (requests & WeatherSchedule::Weather) {
                const auto result = home_weather_refresh(prof.lat, prof.lon); // 阻塞数秒,不持锁
                if (s_home_active.load()) s_weather_schedule.finish(WeatherSchedule::Weather, esp_timer_get_time() / 1000, result);
            }
            if (s_home_active.load() && (requests & WeatherSchedule::Air)) {
                const auto result = home_air_quality_refresh(prof.lat, prof.lon);
                if (s_home_active.load()) s_weather_schedule.finish(WeatherSchedule::Air, esp_timer_get_time() / 1000, result);
            }
            WifiManager::GetInstance().SetPowerSaveLevel(
                home_upload_active() ? WifiPowerSaveLevel::BALANCED : WifiPowerSaveLevel::LOW_POWER);
            s_weather_fetching = false;
            if (!s_home_active.load()) break;
            // 无论成败都要刷新一次:成功换天气图标,失败把 spinner 撤下来
            if (bsp_lvgl_lock(1000)) {
                refresh_weather_ui();
                bsp_lvgl_unlock();
            }
        }

        // 上传模式期间恢复即时下行(手机/设备往返都要快),结束时回到待机省电档。
        // 用电平变化检测,按键开启与 10 分钟自动超时两条路径都覆盖。
        const bool upload_active = home_upload_active();
        if (upload_active != upload_ps_high) {
            upload_ps_high = upload_active;
            WifiManager::GetInstance().SetPowerSaveLevel(
                upload_active ? WifiPowerSaveLevel::BALANCED : WifiPowerSaveLevel::LOW_POWER);
        }

        // 上传模式 10 分钟自动关闭,避免 httpd 长期占着内存。
        if (home_upload_active()) {
            if (upload_started_ms == 0) {
                upload_started_ms = now_ms;
            } else if (now_ms - upload_started_ms >= kUploadTimeoutMs) {
                home_upload_stop();
                upload_started_ms = 0;
            }
        } else {
            upload_started_ms = 0;
        }

        // 1s 周期拆成 10×100ms:stop() 后 ≤100ms 内退出任务,应用切换不再
        // 等满一秒 —— 切入小智/收音机时的过渡黑屏时间显著缩短。
        for (int i = 0; i < 10 && s_home_active.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    s_task_handle = nullptr;
    s_worker_running.store(false);
    vTaskDelete(NULL);
}

void home_init() {
    home_profile_init();
    home_weather_init();
    // 只有 voice 应用调过 time_sync_init();首页自己调一次保证时区已载入(幂等 NVS 读)。
    time_sync_init();

    // LVGL 9 的 header 是位域结构,逐字段赋值比指定初始化器更稳(避免位域初始化顺序问题)。
    // Indexed colors preserve four foreground tones independently of transparency.
    s_portrait_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_portrait_dsc.header.cf = LV_COLOR_FORMAT_I4;
    s_portrait_dsc.header.flags = 0;
    s_portrait_dsc.header.w = HOME_PORTRAIT_W;
    s_portrait_dsc.header.h = HOME_PORTRAIT_H;
    s_portrait_dsc.header.stride = HOME_PORTRAIT_W / 2;
    s_portrait_dsc.data_size = HOME_PORTRAIT_I4_BYTES;
    s_portrait_dsc.data = s_portrait_pixels;

    // 内置底图(A8,数据在 flash,零 RAM 占用)
    const uint8_t *band_data[2] = {kBandArtCity, kBandArtMountain};
    for (int i = 0; i < 2; ++i) {
        s_band_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_band_dsc[i].header.cf = LV_COLOR_FORMAT_A8;
        s_band_dsc[i].header.flags = 0;
        s_band_dsc[i].header.w = BAND_ART_W;
        s_band_dsc[i].header.h = BAND_ART_H;
        s_band_dsc[i].header.stride = BAND_ART_W;
        s_band_dsc[i].data_size = BAND_ART_W * BAND_ART_H;
        s_band_dsc[i].data = band_data[i];
    }

    ESP_LOGI(kTag, "Home app subsystems ready");
}

void home_start() {
    ESP_LOGI(kTag, "Starting Home app");
    home_weather_allow_requests();
    s_home_active.store(true);
    s_ui_alive.store(true);
    s_detail_mode = 0;
    s_share_visible = false;

    // 首屏资料与人像在持锁前读完:SPIFFS 首次挂载可达几十毫秒,不应阻塞 LVGL 任务。
    // Allocate expanded pixels only when an uploaded portrait actually exists.

    home_profile_t prof;
    home_profile_get(&prof);
    const bool has_avatar = home_profile_has_avatar();
    const esp_err_t avatar_err =
        read_portrait(has_avatar);

    bsp_lvgl_lock(-1);
    if (!s_screen) create_ui();
    apply_profile(&prof);
    apply_avatar(avatar_err == ESP_OK);
    update_clock_labels();
    update_battery_label();
    update_wind_line();
    update_footer_label();
    lv_scr_load(s_screen);
    bsp_lvgl_unlock();

    // 栈给 8192 而不是文档里的 4096:本任务内会跑 HTTPS(mbedTLS 握手调用链很深),
    // 4KB 栈在证书校验阶段有溢出风险;另外天气图标走 LVGL TJPEG decoder_info
    // (文件源),它把 4KB 的 TJPGD_WORKBUFF_SIZE 放在栈上(lv_tjpgd.c:110),
    // 6144 在换工具链后实测爆栈(Stack protection fault,2026-09-13)。
    s_worker_running.store(true);
    BaseType_t created = xTaskCreate(home_task, "home_app", 8192, NULL, 4, &s_task_handle);
    if (created != pdPASS) {
        s_worker_running.store(false);
        s_task_handle = nullptr;
        ESP_LOGE(kTag, "Failed to create home task; free heap: %u KB",
                 (unsigned)(esp_get_free_heap_size() / 1024));
        return;
    }
    ESP_LOGI(kTag, "Home app started; free heap: %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
}

bool home_prepare_stop() {
    s_home_active.store(false);
    s_ui_alive.store(false);
    home_weather_cancel_requests();
    return !s_worker_running.load();
}

void home_stop() {
    ESP_LOGI(kTag, "Stopping Home app");
    s_home_active.store(false);
    home_weather_cancel_requests();
    // 先撤掉 UI 开关:home_task 持锁刷新前会看它,避免"删屏后任务还在写标签"。
    s_ui_alive.store(false);

    // Registry calls this only after prepare_stop reports completion. Never
    // delete a task inside DNS/TLS: it may still own heap or socket locks.
    if (!home_prepare_stop()) return;

    home_upload_stop();
    home_clock_stop();

    bsp_lvgl_lock(-1);
    if (s_screen) {
        lv_obj_delete(s_screen);
    }
    s_screen = nullptr;
    s_avatar_img = nullptr;
    s_portrait_ph = nullptr;
    s_lbl_date = nullptr;
    for (lv_obj_t *&bar : s_wifi_bars) bar = nullptr;
    s_batt = {nullptr, nullptr};
    s_band_img = nullptr;
    for (lv_obj_t *&slot : s_clk_slots) slot = nullptr;
    for (lv_obj_t *&dot : s_clk_colon) dot = nullptr;
    std::memset(s_clk_slot_char, 0, sizeof(s_clk_slot_char));
    s_lbl_name = nullptr;
    s_lbl_sig = nullptr;
    s_wicon_zone = nullptr;
    s_wicon_visible = false;
    s_wicon_spinning = false;
    s_lbl_city = nullptr;
    s_lbl_wtext = nullptr;
    s_lbl_temp = nullptr;
    s_lbl_humidity = nullptr;
    s_lbl_wind = nullptr;
    s_lbl_feels = nullptr;
    s_lbl_stat = nullptr;
    s_lbl_footer = nullptr;
    if (s_share_visible) {
        lv_display_t *disp = lv_display_get_default();
        if (disp) lv_display_enable_invalidation(disp, true);
        s_share_visible = false;
    }
    lv_image_cache_drop(&s_portrait_dsc);
    bsp_lvgl_unlock();

    // 屏已删、任务已退,人像缓冲可以还给堆了 —— 其它应用(小智)正等着
    // 这块连续内存开 Opus 编码器。dsc.data 一并清掉,避免悬挂引用。
    if (s_portrait_pixels != nullptr) {
        free(s_portrait_pixels);
        s_portrait_pixels = nullptr;
        s_portrait_dsc.data = nullptr;
    }

    free(s_portrait_packed);
    s_portrait_packed = nullptr;
    ESP_LOGI(kTag, "Home app stopped. Free heap: %u KB",
             (unsigned)(esp_get_free_heap_size() / 1024));
}

void home_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;

    // 手机设置与首次开机共用同一个热点入口。
    if (btn == BSP_BTN_DOWN && event == BTN_EVT_LONG) {
        app_registry_switch_to(APP_ID_WIFI);
        return;
    }

    // 长按 UP = 切换底部艺术底图。
    if (btn == BSP_BTN_UP && event == BTN_EVT_LONG) {
        if (s_share_visible) return;
        bsp_lvgl_lock(-1);
        band_art_next();
        bsp_lvgl_unlock();
        return;
    }

    if (event != BTN_EVT_CLICK) return;

    if (s_share_visible && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        home_share_gallery_t gallery;
        if (home_profile_gallery_get(&gallery) == ESP_OK && gallery.count > 1) {
            const uint8_t previous = s_share_index;
            s_share_index = (s_share_index + gallery.count + (btn == BSP_BTN_UP ? -1 : 1)) % gallery.count;
            if (!home_show_share_image()) {
                s_share_index = previous;
                if (!home_show_share_image()) toggle_share_view();
            }
        }
        return;
    }

    // OK 短按 = 打开应用板块。
    if (btn == BSP_BTN_OK) {
        if (s_share_visible) {
            toggle_share_view();
            return;
        }
        if (s_detail_mode == 2) {
            const int64_t now = esp_timer_get_time() / 1000;
            bsp_lvgl_lock(-1);
            if (now - s_manual_weather_ms >= 30000 && WifiManager::GetInstance().IsConnected()) {
                s_manual_weather_ms = now;
                s_force_weather.store(true);
                footer_set("正在更新天气", &buddy_font_16, kPhos);
            } else {
                footer_set("请稍后再试", &buddy_font_16, kPhos);
            }
            bsp_lvgl_unlock();
            return;
        }
        ESP_LOGI(kTag, "Opening app drawer from Home");
        app_registry_switch_to(APP_ID_LAUNCHER);
        return;
    }

    if (btn == BSP_BTN_UP) {
        toggle_share_view();
        return;
    } else if (btn == BSP_BTN_DOWN) {
        if (s_share_visible) {
            toggle_share_view();
            return;
        }
        // 操作提示、手势、网络/天气、PM2.5 单位及数据来源。
        s_detail_mode = static_cast<uint8_t>((s_detail_mode + 1) % 5);
    } else {
        return;
    }

    bsp_lvgl_lock(-1);
    update_footer_label();
    bsp_lvgl_unlock();
}

}  // namespace

const passport_app_t g_home_app = {
    .id = APP_ID_HOME,
    .name = "首页",
    .en_name = "HOME",
    .desc = "时钟 · 天气 · 名片 · 上传",
    .tag = "直连",
    .theme_color = 0xDDB36C,
    .init = home_init,
    .start = home_start,
    .stop = home_stop,
    .on_key = home_on_key,
    .prepare_stop = home_prepare_stop,
};
