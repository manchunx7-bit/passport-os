// 《你在狗叫什么》声控魔性 App。
//
// 内存策略(上线硬约束,优化后仍然成立):
//   1. 零大缓冲 —— 不用 canvas/framebuffer,狗和管道全部是常驻小对象池
//      (~40 个 lv_obj),游戏过程零堆分配;
//   2. 不另起渲染任务 —— 游戏循环是 33ms 的 lv_timer,跑在 LVGL 任务里;
//      额外任务只有两个:采音(3KB 栈)和音效(2KB 栈,可一键关);
//   3. stop() 全量回收:先删定时器再拆屏,任务 join/删除后返回,堆回到进入前水平。
//
// ---------------------------------------------------------------------------
// 2026-09-13 优化总览(详见同目录 OPTIMIZATION.md):
//   性能  P1 气泡文本不再每帧 lv_label_set_text —— LVGL 9 该接口没有"同文本
//           短路",每次都是 free+malloc+全文重排+invalidate,原来 30 次/秒地
//           制造堆碎片与重排开销,是掉帧与堆抖动的头号来源。改为脏检查。
//        P2 耳朵/嘴/音量条/管道可见性全部走缓存,值没变就不碰 LVGL。
//        P3 修 s_frame 只在 READY 自增(而 READY 永不进入)导致的金丝雀每帧执行。
//        P4 采音:热机期跳过 EMA、首块直接播种(出声快 ~64ms);调试日志可关。
//   结构  S1 拆成 构建 / HUD / 演出 / 循环 四段,px_block·px_label 统一命名;
//        S2 难度与判定常量集中到 game_core.h 的"调参区";
//        S3 音效独立成 game_sfx 模块。
//   手感  F1 死亡坠落 + 结算面板滑入,并按 OK 可快进(不再瞬间重开);
//        F2 修"退出后立刻重进 → 采音任务残留 → 整局没有麦克风"的致命 bug;
//        F3 进场先起麦,热机与建屏重叠。
//   界面  U1 左侧实时音量条(声控游戏最关键的输入可见性);
//        U2 得分闪白;U3 提示语缩短为单行不换行;U4 新增 20 分段位。
// 玩法平衡数值(SPEED/GAP/推力映射/判定盒)全部保持原值,未改动。
// ---------------------------------------------------------------------------
#include "apps/game/app_game.h"

#include "apps/game/game_core.h"
#include "apps/game/game_mic.h"
#include "apps/game/game_sfx.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "launcher/app_registry.h"
#include "ui_pixel.h"
#include "bsp_battery.h"
#include "fonts/app_fonts.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "display/lv_display_private.h" /* canary:inv_p 越界破坏监测 */

#include <atomic>
#include <cstdio>
#include <cstring>

extern "C" {
#include "nvs.h"
}

namespace {

constexpr char kTag[] = "app_game";

// ---------------------------------------------------------------------------
// 调参区(改这里就能试手感;括号内是回退值)
// ---------------------------------------------------------------------------

// 响度→推力映射:实测安静房间 EMA ≈ 60,正常说话数百~一千多,吹气/喊话 >2500。
// 低于 FLOOR 不给推力(自然下落),达到 FULL 满推力;结束日志带本局峰值便于再调。
// 环境吵 → 调高 FLOOR;叫不动 → 调低 FULL。(原值 150 / 2500)
constexpr uint16_t kVolFloor = 150;
constexpr uint16_t kVolFull = 2500;

constexpr uint32_t kFrameMs = 33;

// 音效总开关。置 0 即完全回到"无声版"(game_sfx 不再建任务)。
constexpr bool kSfxEnabled = true;

// 狗嘴:静音 2px → 满声 8px(原值一致)。
constexpr int kMouthMinH = 2;
constexpr int kMouthMaxH = 8;
constexpr int kMouthW = 7;
constexpr int kTongueX = 14;
constexpr int kTongueYBase = 8;

// 气泡阈值(推力 0..255):>60 出"汪!",≥180 升级"汪汪!"(原值一致)。
constexpr int kBarkSoft = 60;
constexpr int kBarkLoud = 180;

// 得分闪白时长(帧)。6 ≈ 200ms。
constexpr int kScoreFlashFrames = 6;

// 死亡演出:坠落 ~300ms(9 帧 × 7px),随后面板滑入 ~230ms(7 帧)。
// 回退:把 kDeathFallFrames 设 0 即为"立刻弹结算"的原行为。
constexpr int kDeathFallFrames = 9;
constexpr int kDeathFallPx = 7;
constexpr int kPanelAnimFrames = 7;
constexpr int kPanelShownY = 86;
constexpr int kPanelHiddenY = 340;

// 左侧实时音量条:让玩家看见"麦克风听到多少",声控游戏最重要的输入可见性。
constexpr int kVuX = 4;
constexpr int kVuY = 34;
constexpr int kVuW = 6;
constexpr int kVuH = 244;    // 34..278,不压到草地(GC_GROUND_Y=284)
constexpr int kVuStep = 6;   // 高度量化步长,减少无意义的重绘

// 提示语(单行,不换行 —— 原来 13 个汉字宽 221px 超过 216 会折成两行压在草地上)
constexpr char kHintPlay[] = "出声升 · 安静降 · 确定跳";
constexpr char kHintDead[] = "确定再来 · 长按确定返回";
constexpr char kHintReady[] = "确定开始 · 长按确定返回";

// ---------------------------------------------------------------------------
// 配色(沿用整机像素风色板)
// ---------------------------------------------------------------------------
#define kInk (UI_INK)
#define kPaper (UI_PAPER)
constexpr uint32_t kDogBody = 0xE6A13B;  // 柴犬金黄毛色
constexpr uint32_t kDogEar = 0xAA6518;   // 褐色耳朵
constexpr uint32_t kDogWhite = 0xFFF3D6; // 脸颊浅白
constexpr uint32_t kMouthBg = 0x551111;  // 深暗红嘴腔
constexpr uint32_t kTongue = 0xFF6B8B;   // 粉舌头
#define kGrass (UI_ACCENT)    // 管道主体
#define kGrassD (UI_BORDER)   // 管道口/描边

// ---------------------------------------------------------------------------
// 文本缓存 —— 本次优化的核心
// ---------------------------------------------------------------------------

// lv_label_set_text() 内部没有"和当前文本相同就直接返回"的短路:每次调用都会
// lv_free 旧串 → lv_malloc 新串 → 全文重新排版 → invalidate。原来"汪!"气泡每帧
// 无条件调用一次(30 次/秒),既制造堆碎片又反复触发布局。这里在调用前先比对。
struct TextCache {
    char text[48];
    void reset() { text[0] = '\0'; }
};

bool set_text_cached(lv_obj_t *label, TextCache &cache, const char *text) {
    if (!label || !text) return false;
    if (std::strcmp(cache.text, text) == 0) return false;  // 没变:一次都不碰 LVGL
    std::snprintf(cache.text, sizeof(cache.text), "%s", text);
    lv_label_set_text(label, cache.text);
    return true;
}

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
std::atomic<bool> s_active{false};
lv_obj_t *s_screen = nullptr;
lv_timer_t *s_timer = nullptr;

lv_obj_t *s_dog = nullptr;               // 容器 22x18
lv_obj_t *s_dog_ear = nullptr;           // 可摆动后耳
lv_obj_t *s_dog_mouth = nullptr;         // 动态拉伸嘴腔
lv_obj_t *s_dog_tongue = nullptr;        // 吐舌头
lv_obj_t *s_bark_lbl = nullptr;          // 实时“汪！”气泡
lv_obj_t *s_score_lbl = nullptr;
ui_pixel_battery_t s_battery{};
int64_t s_battery_us = 0;
lv_obj_t *s_hint_lbl = nullptr;
lv_obj_t *s_vu_fill = nullptr;           // 音量条填充(轨道是它的兄弟,不需句柄)
lv_obj_t *s_dead_panel = nullptr;
lv_obj_t *s_dead_score = nullptr;
lv_obj_t *s_dead_best = nullptr;
lv_obj_t *s_dead_rating = nullptr;       // 抽象段位评级

// 渲染缓存:值没变就不写 LVGL。lv_obj_set_x/y 本身有相同值短路,但
// set_size / set_style / set_text 没有,统一在这里挡一层最省心。
int s_last_dog_y = -1;
int s_last_mouth_h = -1;
int s_last_ear_y = -1;
int s_last_vu_h = -1;
uint32_t s_last_vu_color = 0;
int s_dog_y = 0;                          // 最近一次渲染用的狗 y(死亡坠落起点)

// 管道池:每根 = 上身/上口/下口/下身 4 个对象,循环复用
lv_obj_t *s_pipe_obj[GC_PIPES][4];
int s_pipe_last_h[GC_PIPES][2];      // 上身/下身缓存高度,-1 强制首次刷新
bool s_pipe_shown[GC_PIPES];         // 上一帧是否在屏内(避免离屏管道反复被搬家)

// 演出 / 计分 / 统计
int s_score_cache = -1;
int s_score_flash = 0;
int s_dying = 0;        // >0:正在演坠落;0:可重开
int s_panel_anim = 0;   // >0:结算面板正在滑入
int s_best = 0;
uint16_t s_peak_level = 0;
uint32_t s_frame = 0;

TextCache s_bark_cache;
TextCache s_hint_cache;

// 掉帧统计(放到文件作用域,start_run 能清零,避免跨局残留)
int64_t s_last_tick_us = 0;
int32_t s_max_tick_ms = 0;
int s_slow_frames = 0;
int s_stat_frames = 0;

// 呼吸浮动表(READY 态):±6px,周期约 1s
const int8_t kBob[16] = {0, 2, 4, 5, 6, 5, 4, 2, 0, -2, -4, -5, -6, -5, -4, -2};

// ---------------------------------------------------------------------------
// 构件 —— 统一前缀 px_*,和整机像素风一致
// ---------------------------------------------------------------------------
lv_obj_t *px_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color,
                   int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    // 显式不透明:省掉每次绘制的 alpha 混合,纯色块是本机最便宜的图元。
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t *px_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color, lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

// ---------------------------------------------------------------------------
// 构建
// ---------------------------------------------------------------------------
void build_dog(lv_obj_t *parent) {
    s_dog = lv_obj_create(parent);
    lv_obj_remove_flag(s_dog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_dog, GC_BIRD_X, 140);
    lv_obj_set_size(s_dog, GC_BIRD_W, GC_BIRD_H);
    lv_obj_set_style_bg_opa(s_dog, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dog, 0, 0);
    lv_obj_set_style_pad_all(s_dog, 0, 0);

    // 1. 头身主体(金黄, 4px 圆角)
    px_block(s_dog, 2, 2, 17, 13, kDogBody, 4);
    // 2. 左耳朵(后耳, 飞行动画微晃)
    s_dog_ear = px_block(s_dog, 0, 1, 5, 7, kDogEar, 2);
    // 3. 右耳朵(前耳)
    px_block(s_dog, 6, 0, 5, 6, kDogEar, 2);
    // 4. 浅色脸颊/口鼻区
    px_block(s_dog, 10, 6, 10, 8, kDogWhite, 2);
    // 5. 黑鼻头
    px_block(s_dog, 18, 5, 4, 3, kInk, 1);
    // 6. 呆萌眼睛(大白底 + 黑瞳孔)
    px_block(s_dog, 9, 3, 5, 5, 0xFFFFFF, 2);
    px_block(s_dog, 11, 4, 2, 2, kInk, 1);
    // 7. 动态嘴腔(暗红, 初始高 2)
    s_dog_mouth = px_block(s_dog, 13, 9, kMouthW, kMouthMinH, kMouthBg, 1);
    // 8. 舌头(粉色, 初始附在嘴里)
    s_dog_tongue = px_block(s_dog, kTongueX, kTongueYBase + kMouthMinH, 5, 2, kTongue, 1);

    // 9. 狗叫冒泡气泡标签 (显示在头顶上方)
    s_bark_lbl = px_label(parent, "", &buddy_font_16,
        ui_theme_get() == UI_THEME_PLAYFUL ? UI_WARNING : 0xFFE066, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_bark_lbl, GC_BIRD_X + GC_BIRD_W + 2, 134);
}

void build_pipes(lv_obj_t *parent) {
    for (int i = 0; i < GC_PIPES; ++i) {
        s_pipe_obj[i][0] = px_block(parent, -40, 0, GC_PIPE_W, 10, kGrass, 0);
        s_pipe_obj[i][1] = px_block(parent, -40, 0, GC_PIPE_W + GC_PIPE_LIP_OVERHANG * 2,
                                    GC_PIPE_LIP, kGrassD, 0);
        s_pipe_obj[i][2] = px_block(parent, -40, 0, GC_PIPE_W + GC_PIPE_LIP_OVERHANG * 2,
                                    GC_PIPE_LIP, kGrassD, 0);
        s_pipe_obj[i][3] = px_block(parent, -40, 0, GC_PIPE_W, 10, kGrass, 0);
        s_pipe_last_h[i][0] = -1;
        s_pipe_last_h[i][1] = -1;
        s_pipe_shown[i] = false;
    }
}

// 音量条:轨道 + 填充。填充初始透明,update_vu() 按推力点亮。
void build_vu(lv_obj_t *parent) {
    px_block(parent, kVuX, kVuY, kVuW, kVuH, UI_SURFACE_ALT, 0);
    s_vu_fill = px_block(parent, kVuX, kVuY, kVuW, kVuH, UI_YELLOW, 0);
    lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_TRANSP, 0);
}

void build_dead_panel(lv_obj_t *parent) {
    // 纸底 + 墨描边,宽 190, 高 122(几何与优化前完全一致,只加了滑入动画)
    lv_obj_t *panel = px_block(parent, 12, kPanelHiddenY, 216, 140, kPaper, 0);
    s_dead_panel = panel;
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_side(panel, ui_theme_is_classic()
        ? LV_BORDER_SIDE_FULL
        : static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_BORDER), 0);

    lv_obj_t *title = px_label(panel, "再来一局？", &ui_font_20, UI_TEXT,
                               LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(title, 210);
    lv_obj_set_pos(title, 3, 10);

    s_dead_score = px_label(panel, "0", &ui_digits_32, UI_SKY_DARK,
                            LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_score, 210);
    lv_obj_set_pos(s_dead_score, 3, 34);

    s_dead_best = px_label(panel, "最高纪录 0", &buddy_font_16, UI_SUB,
                           LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_best, 210);
    lv_obj_set_pos(s_dead_best, 3, 82);

    s_dead_rating = px_label(panel, "轻声挑战，下次再试", &buddy_font_16, UI_TEXT,
                             LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_rating, 210);
    lv_obj_set_pos(s_dead_rating, 3, 112);
}

// ---------------------------------------------------------------------------
// HUD —— 全部"脏检查"驱动
// ---------------------------------------------------------------------------
void set_dog_y(int y) {
    if (y == s_last_dog_y) return;
    s_last_dog_y = y;
    s_dog_y = y;
    lv_obj_set_y(s_dog, y);
}

// 气泡:只有文本真正变化时才 lv_label_set_text;非空时跟着狗走(并避开顶栏)。
void update_bark(const char *text, int dog_y) {
    set_text_cached(s_bark_lbl, s_bark_cache, text);
    if (text[0] == '\0') return;
    int y = dog_y - 4;
    if (y < GC_TOP + 2) y = GC_TOP + 2;
    lv_obj_set_y(s_bark_lbl, y);
}

// 音量条:高度量化后再比较,静音时整体透明(不用 HIDDEN —— 见 sync_pipes 注释)。
void update_vu(int thrust) {
    int h = (thrust * kVuH) / 255;
    h = (h / kVuStep) * kVuStep;
    if (h < kVuStep) h = 0;
    if (h == s_last_vu_h) return;

    if (h == 0) {
        lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_TRANSP, 0);
    } else {
        if (s_last_vu_h <= 0) lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_COVER, 0);
        lv_obj_set_size(s_vu_fill, kVuW, h);
        lv_obj_set_y(s_vu_fill, kVuY + (kVuH - h));   // 从底部往上长
        const uint32_t c = (thrust < 85) ? UI_GRASS
                                         : (thrust < kBarkLoud ? UI_YELLOW : UI_RED);
        if (c != s_last_vu_color) {
            lv_obj_set_style_bg_color(s_vu_fill, lv_color_hex(c), 0);
            s_last_vu_color = c;
        }
    }
    s_last_vu_h = h;
}

void set_hint(const char *text) { set_text_cached(s_hint_lbl, s_hint_cache, text); }

void sync_pipes() {
    for (int i = 0; i < GC_PIPES; ++i) {
        const gc_pipe_t *p = gc_pipe(i);
        const int px = (int)(p->x >> 4);
        const bool visible = px > -(GC_PIPE_W + 10) && px < 240;
        lv_obj_t **o = s_pipe_obj[i];
        if (!visible) {
            /* 不用 HIDDEN 标志:LVGL 9.3 取消隐藏路径在本机触发过 inv_p 死循环
               (见 tick 金丝雀),与结算面板同款"移出屏幕"方案。
               加 s_pipe_shown 缓存后,离屏期间一次都不用再写 LVGL。 */
            if (s_pipe_shown[i]) {
                for (int k = 0; k < 4; ++k) lv_obj_set_pos(o[k], -40, 0);
                s_pipe_shown[i] = false;
            }
            continue;
        }
        s_pipe_shown[i] = true;
        const int gap_top = (int)(p->gap_y >> 4);
        const int gap_bot = gap_top + (int)p->gap_h;
        const int lip = GC_PIPE_LIP_OVERHANG;

        lv_obj_set_pos(o[0], px, GC_TOP);
        lv_obj_set_pos(o[1], px - lip, gap_top - GC_PIPE_LIP);
        lv_obj_set_pos(o[2], px - lip, gap_bot);
        lv_obj_set_pos(o[3], px, gap_bot + GC_PIPE_LIP);

        const int top_h = gap_top - GC_TOP;
        if (s_pipe_last_h[i][0] != top_h) {          // 高度只在回收时变化,按需 set
            lv_obj_set_size(o[0], GC_PIPE_W, top_h);
            s_pipe_last_h[i][0] = top_h;
        }
        const int bot_h = GC_GROUND_Y - gap_bot - GC_PIPE_LIP;
        if (s_pipe_last_h[i][1] != bot_h) {
            lv_obj_set_size(o[3], GC_PIPE_W, bot_h);
            s_pipe_last_h[i][1] = bot_h;
        }
    }
}

// ---------------------------------------------------------------------------
// 最高纪录(NVS)
// ---------------------------------------------------------------------------
void load_best(int *best) {
    *best = 0;
    nvs_handle_t h;
    if (nvs_open("flappy", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "best", &v) == ESP_OK) *best = v;
        nvs_close(h);
    }
}

void save_best(int best) {
    nvs_handle_t h;
    if (nvs_open("flappy", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, "best", (uint8_t)best) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// 一局的开始 / 结束
// ---------------------------------------------------------------------------
void start_run() {
    /* 进 app 直接起飞 / 死后一键重开:不再有待机的 READY 静止页,
       否则进场看到的是"原地浮动+空屏",像没运行(用户反馈过)。 */
    s_score_cache = -1;
    s_score_flash = 0;
    s_dying = 0;
    s_panel_anim = 0;
    s_peak_level = 0;
    s_last_tick_us = 0;
    s_max_tick_ms = 0;
    s_slow_frames = 0;
    s_stat_frames = 0;

    lv_label_set_text(s_score_lbl, "0");
    lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(UI_YELLOW), 0);
    if (s_dead_panel) lv_obj_set_y(s_dead_panel, kPanelHiddenY);

    // 渲染缓存全部置无效,保证新一局第一帧把狗/嘴/耳/音量条刷成初始态
    s_last_dog_y = -1;
    s_last_mouth_h = -1;
    s_last_ear_y = -1;
    s_last_vu_h = -1;
    s_last_vu_color = 0;
    s_bark_cache.reset();
    if (s_bark_lbl) lv_label_set_text(s_bark_lbl, "");
    if (s_vu_fill) lv_obj_set_style_bg_opa(s_vu_fill, LV_OPA_TRANSP, 0);
    for (int i = 0; i < GC_PIPES; ++i) {
        s_pipe_last_h[i][0] = -1;
        s_pipe_last_h[i][1] = -1;
        s_pipe_shown[i] = true;   // 下一帧 sync_pipes 会按真实可见性纠正
    }

    gc_start((uint32_t)esp_timer_get_time() & 0x7fffffffu);
    set_hint(kHintPlay);
    if (kSfxEnabled) game_sfx_play(GS_START);
}

void on_dead() {
    const int score = gc_score();
    const bool new_best = score > s_best;
    if (new_best) {
        s_best = score;
        save_best(s_best);
    }
    char line[24];
    std::snprintf(line, sizeof(line), "%d", score);
    lv_label_set_text(s_dead_score, line);
    std::snprintf(line, sizeof(line), "最高纪录 %d", s_best);
    lv_label_set_text(s_dead_best, line);

    // 抽象段位评级(沿用原四档,补一档 20 分彩蛋,强化成就感)
    const char *rating = "轻声挑战，下次再试";
    if (score >= 20) rating = "全小区的狗都跟着你叫";
    else if (score >= 10) rating = "纯种哮天犬现世";
    else if (score >= 5) rating = "保安已被你惊动";
    else if (score >= 2) rating = "单身狗·很有精神";
    if (s_dead_rating) lv_label_set_text(s_dead_rating, rating);

    // 死亡演出:先张嘴"呜"着往下坠,结算面板等坠完再滑入(内容已填好)
    s_dying = kDeathFallFrames;
    s_panel_anim = 0;
    if (s_dead_panel) lv_obj_set_y(s_dead_panel, kPanelHiddenY);
    lv_obj_set_size(s_dog_mouth, kMouthW, kMouthMaxH);
    lv_obj_set_pos(s_dog_tongue, kTongueX, kTongueYBase + kMouthMaxH);
    s_last_mouth_h = kMouthMaxH;
    lv_obj_set_y(s_dog_ear, 3);      // 耳朵彻底耷拉
    s_last_ear_y = 3;
    update_bark("呜!!", s_dog_y);
    set_hint(kHintDead); /* 明确告诉玩家怎么继续,免得以为死机 */
    if (kSfxEnabled) game_sfx_play(new_best ? GS_BEST : GS_DEAD);

    static const char *const kReason[] = {"none", "ground", "pipe-top", "pipe-bottom"};
    const gc_death_info_t d = gc_death_info();
    ESP_LOGI(kTag, "game over: score=%d best=%d peak=%u death=%s dog_y=%ld pipe_x=%ld "
                   "gap_top=%ld gap_h=%ld",
             score, s_best, s_peak_level, kReason[d.reason], (long)d.bird_y,
             (long)d.pipe_x, (long)d.gap_top, (long)d.gap_h);
}

// ---------------------------------------------------------------------------
// 每帧三段循环
// ---------------------------------------------------------------------------
void ready_tick() {
    // 悬浮呼吸;顺便每秒打一条响度采样,便于校准声控阈值
    const int bob = kBob[(s_frame >> 1) & 15];
    const int dog_y = ((GC_TOP + GC_GROUND_Y) / 2 - GC_BIRD_H / 2) + bob - 3;
    set_dog_y(dog_y);
    update_bark("", dog_y);
    if ((s_frame % 30) == 0) {
        ESP_LOGI(kTag, "mic level=%u", game_mic_level());
    }
}

void playing_tick() {
    app_registry_keep_awake(); /* 纯声控操作,进行中必须抑制息屏 */
    const uint16_t level = game_mic_level();
    if (level > s_peak_level) s_peak_level = level;
    int thrust = ((int)level - kVolFloor) * 255 / (kVolFull - kVolFloor);
    if (thrust < 0) thrust = 0;
    if (thrust > 255) thrust = 255;
    gc_set_thrust((uint8_t)thrust);
    gc_step();

    const int dog_y = (int)(gc_bird_y_q4() >> 4);
    set_dog_y(dog_y);

    // 嘴巴随声音动态张开(P2:高度不变就不写 LVGL)
    const int mouth_h = kMouthMinH + thrust * (kMouthMaxH - kMouthMinH) / 255;
    if (mouth_h != s_last_mouth_h) {
        lv_obj_set_size(s_dog_mouth, kMouthW, mouth_h);
        lv_obj_set_pos(s_dog_tongue, kTongueX, kTongueYBase + mouth_h);
        s_last_mouth_h = mouth_h;
    }

    // 耳朵晃动:上升时耳朵微翘,下落时下垂(只在翻转的那一帧写)
    const int ear_y = (gc_bird_vy_q4() < 0) ? 0 : 2;
    if (ear_y != s_last_ear_y) {
        lv_obj_set_y(s_dog_ear, ear_y);
        s_last_ear_y = ear_y;
    }

    // 叫声弹幕气泡(P1:只在跨档位时改文本,不再每帧 malloc+重排)
    const char *bark = "";
    if (thrust >= kBarkLoud) bark = "汪汪!";
    else if (thrust > kBarkSoft) bark = "汪!";
    update_bark(bark, dog_y);

    update_vu(thrust);
    sync_pipes();

    if (gc_score() != s_score_cache) {
        s_score_cache = gc_score();
        char text[12];
        std::snprintf(text, sizeof(text), "%d", s_score_cache);
        lv_label_set_text(s_score_lbl, text);
        // 得分闪白 ~200ms:让"过了一根"这件事在余光里也能看见
        lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(UI_TEXT), 0);
        s_score_flash = kScoreFlashFrames;
        if (kSfxEnabled) game_sfx_play(GS_SCORE);
    }
    if (s_score_flash > 0 && --s_score_flash == 0) {
        lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(UI_YELLOW), 0);
    }

    if (gc_phase() == GC_DEAD) on_dead();
}

void dead_tick() {
    if (s_panel_anim > 0) {                        // 结算面板滑入
        --s_panel_anim;
        const int done = kPanelAnimFrames - s_panel_anim;
        const int y = kPanelHiddenY +
                      (kPanelShownY - kPanelHiddenY) * done / kPanelAnimFrames;
        lv_obj_set_y(s_dead_panel, y);
        return;
    }
    if (s_dying > 0) {                             // 狗继续往下坠(纯视觉,逻辑已停)
        --s_dying;
        int y = s_dog_y + kDeathFallPx;
        const int floor_y = GC_GROUND_Y - GC_BIRD_H;
        if (y > floor_y) y = floor_y;
        set_dog_y(y);
        update_bark("呜!!", y);
        if (s_dying == 0) s_panel_anim = kPanelAnimFrames;
    }
}

void tick(lv_timer_t *) {
    if (!s_active.load()) return;
    const bool visible = app_registry_screen_is_on();
    const bool playing = gc_phase() == GC_PLAYING;
    if (s_timer) lv_timer_set_period(s_timer, visible || playing ? kFrameMs : 1000);
    if (!visible && !playing) { s_last_tick_us = 0; return; }

    const int64_t now = esp_timer_get_time();
    if (now - s_battery_us >= 5000000) { ui_pixel_battery_set(&s_battery, bsp_battery_soc()); s_battery_us = now; }
    if (s_last_tick_us) {
        const int64_t dt_ms = (now - s_last_tick_us) / 1000;
        if (dt_ms > 45) ++s_slow_frames;
    }
    s_last_tick_us = now;

    // P3:s_frame 原来只在 READY 分支自增,而游戏进场即起飞、READY 永不进入 →
    // s_frame 恒为 0 → 下面的金丝雀检测每帧都跑。改为每帧自增。
    ++s_frame;

    switch (gc_phase()) {
    case GC_READY:   ready_tick();   break;
    case GC_PLAYING: playing_tick(); break;
    case GC_DEAD:    dead_tick();    break;
    }

    const int64_t spent_ms = (esp_timer_get_time() - now) / 1000;
    if (spent_ms > s_max_tick_ms) s_max_tick_ms = (int32_t)spent_ms;
    if (++s_stat_frames >= 30) { /* ~1s 汇总:只在掉帧异常时才出日志,平时安静 */
        if (s_slow_frames >= 5) {
            ESP_LOGW(kTag, "frame drops: %d/%d max_tick=%ldms", s_slow_frames,
                     s_stat_frames, (long)s_max_tick_ms);
        }
        s_slow_frames = 0;
        s_stat_frames = 0;
        s_max_tick_ms = 0;
    }

    /* 金丝雀:LVGL 9.3 lv_inv_area 里 uint16 i 对 uint32 inv_p,inv_p 一旦被
       OOB 写坏成 >65535,i 回绕 → 死循环(真机 WDT 抓到过现场)。只在异常时告警。 */
    if ((s_frame & 31) == 0) {
        lv_display_t *disp = lv_display_get_default();
        if (disp && disp->inv_p > LV_INV_BUF_SIZE) {
            ESP_LOGE(kTag, "CANARY: inv_p corrupted=%u", (unsigned)disp->inv_p);
        }
    }
}

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------
void build_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    ui_pixel_background(s_screen);

    lv_obj_t *bar = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = px_label(bar, "你在狗叫什么", &buddy_font_16, UI_TEXT,
                               LV_TEXT_ALIGN_LEFT);
    ui_pixel_status_text_align(title, 12, 96, LV_TEXT_ALIGN_LEFT);
    s_score_lbl = px_label(bar, "0", &lv_font_montserrat_14, UI_YELLOW,
                           LV_TEXT_ALIGN_RIGHT);
    ui_pixel_status_text_align(s_score_lbl, 110, UI_STATUS_CONTENT_RIGHT - 110,
                               LV_TEXT_ALIGN_RIGHT);
    s_battery = ui_pixel_battery_create(bar, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc()); s_battery_us = esp_timer_get_time();

    build_pipes(s_screen);
    build_vu(s_screen);
    build_dog(s_screen);

    // 提示语:纸胶囊(设计系统同款方角 + 墨描边)。
    // 原来是纯白字直接压在草地上,白/草绿对比只有约 2.2:1,看不清;
    // 换成纸底 + UI_SUB 后约 6:1,同时和整机其它 App 的提示胶囊一致。
    s_hint_lbl = px_label(s_screen, "", &buddy_font_16, UI_SUB, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_hint_lbl, 216);
    lv_obj_set_style_bg_color(s_hint_lbl, lv_color_hex(kPaper), 0);
    lv_obj_set_style_bg_opa(s_hint_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_hint_lbl, 4, 0);
    lv_obj_set_style_radius(s_hint_lbl, 0, 0);
    lv_obj_set_style_border_width(s_hint_lbl, 0, 0);
    lv_obj_set_style_border_color(s_hint_lbl, lv_color_hex(UI_BORDER), 0);
    lv_obj_align(s_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);

    build_dead_panel(s_screen);

    load_best(&s_best);
    s_frame = 0;
    s_peak_level = 0;
    s_hint_cache.reset();
    start_run(); /* 进入即起飞,页面立刻是"往前飞"的动态 */
}

}  // namespace

extern "C" {

static void game_init(void) {}

static void game_start(void) {
    if (s_active.load()) return;
    s_active.store(true);
    /* F3:先起采音再建屏 —— 麦克风有 ~160ms 的上电瞬态要丢,和建 40 个 lv_obj
       的时间重叠掉,进场第一帧就不会因为"电平还没出来"而先往下沉一截。 */
    game_mic_start();
    if (kSfxEnabled) game_sfx_start();
    bsp_lvgl_lock(-1);
    build_ui();                       // 建屏必须持锁:start 跑在输入任务上下文
    lv_screen_load(s_screen);
    s_timer = lv_timer_create(tick, kFrameMs, nullptr);
    bsp_lvgl_unlock();
    ESP_LOGI(kTag, "started; free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static bool game_prepare_stop(void) {
    s_active.store(false);
    const bool sound_done = !kSfxEnabled || game_sfx_prepare_stop();
    const bool mic_done = game_mic_prepare_stop();
    return sound_done && mic_done;
}

static void game_stop(void) {
    s_active.store(false);
    // 音效任务先收(它可能在阻塞写 I2S),再收采音,最后拆屏 —— 顺序不能反。
    if (kSfxEnabled) game_sfx_stop();
    // 2026-09-13 修复(B2):删屏原来写在 if (s_timer) 里面 —— 堆紧张时
    // lv_timer_create 返回 NULL,整个分支被跳过,screen 与全部子对象永不释放。
    // 现在定时器与界面分开处理,屏幕**无条件**删。
    bsp_lvgl_lock(-1);
    if (s_timer) {              // 先删定时器,防止 tick 撕屏途中再进来
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    bsp_lvgl_unlock();
    s_dog = s_dog_ear = s_dog_mouth = s_dog_tongue = s_bark_lbl = nullptr;
    s_score_lbl = s_hint_lbl = s_vu_fill = nullptr;
    s_dead_panel = s_dead_score = s_dead_best = s_dead_rating = nullptr;
    for (int i = 0; i < GC_PIPES; ++i)
        for (int k = 0; k < 4; ++k) s_pipe_obj[i][k] = nullptr;
    game_mic_stop();
    bsp_audio_deinit();
    ESP_LOGI(kTag, "stopped; free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void game_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_active.load() || !s_screen || event != BTN_EVT_CLICK) return;
    if (btn != BSP_BTN_OK && btn != BSP_BTN_UP) return;

    /* 本回调跑在输入任务:set_hint 等 lv_* 调用必须持锁。开局瞬间无锁
       invalidate 与渲染任务竞态,会把 disp->inv_p 写坏成 >65535,
       lv_inv_area 的 uint16 循环变量回绕 → LVGL 死循环(真机 WDT 抓到过)。
       (LVGL 的 lv_timer 回调同样持这把锁,所以 tick 与本函数天然互斥,
       下面这些非原子状态不需要额外加锁。) */
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) lv_timer_set_period(s_timer, kFrameMs);

    switch (gc_phase()) {
    case GC_READY:
        start_run(); /* 理论上不再进入 READY,兜底直接起飞 */
        break;
    case GC_PLAYING:
        gc_flap();
        break;
    case GC_DEAD:
        if (s_dying > 0) {
            /* 还在演坠落:这一下 OK 当作"快进",直接进结算,而不是重开 ——
               否则连按两下就瞬间重开,玩家根本来不及看到自己几分。 */
            s_dying = 0;
            s_panel_anim = kPanelAnimFrames;
        } else {
            start_run(); /* 死亡面板上按 OK 立刻再来一局 */
        }
        break;
    }

    bsp_lvgl_unlock();
}

const passport_app_t g_game_app = {
    .id = APP_ID_GAME,
    .name = "你在狗叫什么",
    .en_name = "WHY BARKING",
    .desc = "出声上升，安静下降\n用声音越过障碍",
    .tag = "DOG",
    .theme_color = 0xEEA438,
    .init = game_init,
    .start = game_start,
    .stop = game_stop,
    .on_key = game_on_key,
    .prepare_stop = game_prepare_stop,
};
}
