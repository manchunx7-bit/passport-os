#include "apps/game/app_tetris.h"

#include "apps/game/tetris_core.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "launcher/app_registry.h"
#include "ui_pixel.h"
#include "fonts/app_fonts.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <atomic>
#include <cstdio>

extern "C" {
#include "nvs.h"
}

namespace {

constexpr char kTag[] = "app_tetris";

/* ---------------------------------------------------------------------------
   渲染方式(2026-09-13 重写)
   原来用一整块 canvas 帧缓冲:10*cell × 20*cell × 2B。cell=14 需要 78.4KB
   **连续**内存,而真机实测 Tetris 启动时 largest 只有 53KB(堆碎片),
   结果一路降级到 cell=10(100x200 —— 比屏幕一半还窄,这就是"游戏区很小")。
   切片也救不了:空闲空间基本集中在同一块里,切 4 条只是把同一块切成 4 份。

   改成 **200 个 lv_obj 小方块网格**:每格一个对象,颜色用**共享静态样式**
   切换(不产生逐对象局部样式)。单块内存只占 ~110B,碎片堆也放得下,总占用
   (~22KB)反而比原来 40KB 的帧缓冲更小。每次移动只有个位数格子变色,
   LVGL 只重绘那些小区域。
   --------------------------------------------------------------------------- */

int s_cell;
ui_pixel_battery_t s_battery{};
int64_t s_battery_us = 0;
int s_field_w;
int s_field_h;
int s_panel_x;                /* 右侧信息列起点,随格子尺寸联动 */
constexpr uint32_t kTickMs = 50;

/* 方块配色(与整机调色板同源);下标 = tc_board 值 */
static uint32_t kPieceColors[TC_PIECE_KINDS + 1] = {
    0, /* 0 空格子底色(深暗岩灰,无眩光,与纯黑网格线形成高对比) */
    0x1689E8, /* 1 I 天蓝 */
    0xFFD928, /* 2 O 黄 */
    0x7557D9, /* 3 T 紫 */
    0x82BE2D, /* 4 S 绿 */
    0xE43B2F, /* 5 Z 红 */
    0x0872C9, /* 6 J 深蓝 */
    0xFFB23E, /* 7 L 橙 */
};

/* 共享静态样式:每色一份,全部格子复用 —— 不产生逐对象局部样式分配 */
lv_style_t s_cell_styles[TC_PIECE_KINDS + 1];
bool s_styles_ready = false;

std::atomic<bool> s_active{false};
lv_timer_t *s_timer = nullptr;
lv_obj_t *s_screen = nullptr;
lv_obj_t *s_board = nullptr;                        /* 棋盘容器(底色 = 棋盘墨色) */
lv_obj_t *s_grid[TC_ROWS][TC_COLS] = {};            /* 格子对象 */
uint8_t s_shown[TC_ROWS][TC_COLS] = {};             /* 已显示的颜色索引,用于差分 */
lv_obj_t *s_score_lbl = nullptr;
lv_obj_t *s_val_score = nullptr;
lv_obj_t *s_val_lines = nullptr;
lv_obj_t *s_val_level = nullptr;
lv_obj_t *s_val_best = nullptr;
lv_obj_t *s_next_blk[4];
lv_obj_t *s_controls;
lv_obj_t *s_hint_lbl = nullptr;
lv_obj_t *s_dead_panel = nullptr;
lv_obj_t *s_dead_info = nullptr;
lv_obj_t *s_dead_best = nullptr;

int s_score_cache = -1;
int s_lines_cache = -1;
int s_best = 0;
int32_t s_gravity_acc;
uint32_t s_frame;

lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    if (!o) return nullptr;
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

/* 格子对象:预设为 0 号样式(暗底+方角+无边距),消除 LVGL 默认主题产生的圆角白点 */
lv_obj_t *cell_obj(lv_obj_t *parent, int x, int y, int sz) {
    lv_obj_t *o = lv_obj_create(parent);
    if (!o) return nullptr;
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, sz, sz);
    lv_obj_add_style(o, &s_cell_styles[0], 0);
    return o;
}

lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color, lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

void ensure_styles() {
    kPieceColors[0] = UI_SURFACE_ALT;
    if (s_styles_ready) {
        lv_style_set_bg_color(&s_cell_styles[0], lv_color_hex(kPieceColors[0]));
        return;
    }
    for (int i = 0; i <= TC_PIECE_KINDS; ++i) {
        lv_style_init(&s_cell_styles[i]);
        lv_style_set_radius(&s_cell_styles[i], 0);
        lv_style_set_border_width(&s_cell_styles[i], 0);
        lv_style_set_pad_all(&s_cell_styles[i], 0);
        lv_style_set_bg_opa(&s_cell_styles[i], LV_OPA_COVER);
        lv_style_set_bg_color(&s_cell_styles[i], lv_color_hex(kPieceColors[i]));
    }
    s_styles_ready = true;
}

/* 单格换色(差分:颜色没变就完全不碰 LVGL) */
void cell_set(int col, int row, uint8_t idx) {
    if (col < 0 || col >= TC_COLS || row < 0 || row >= TC_ROWS) return;
    const uint8_t prev = s_shown[row][col];
    if (prev == idx) return;
    lv_obj_t *o = s_grid[row][col];
    if (!o) return;
    lv_obj_remove_style(o, &s_cell_styles[prev], 0);
    lv_obj_add_style(o, &s_cell_styles[idx], 0);
    s_shown[row][col] = idx;
}

/* 整板重绘:先算目标色板,再逐格差分 —— 只有真正变色的格子才会触发重绘 */
void render_all() {
    if (!s_board) return;
    uint8_t target[TC_ROWS][TC_COLS];
    const uint8_t *bd = tc_board();
    for (int r = 0; r < TC_ROWS; ++r)
        for (int c = 0; c < TC_COLS; ++c) target[r][c] = bd[r * TC_COLS + c];
    int8_t cells[4][2];
    const int cur = tc_cur_id();
    if (cur && tc_cur_cells(cells)) {
        for (int i = 0; i < 4; ++i)
            if (cells[i][1] >= 0) target[cells[i][1]][cells[i][0]] = (uint8_t)cur;
    }
    for (int r = 0; r < TC_ROWS; ++r)
        for (int c = 0; c < TC_COLS; ++c) cell_set(c, r, target[r][c]);
}

void render_next() {
    int8_t cells[4][2];
    tc_next_cells(cells);
    for (lv_obj_t *cell : s_next_blk)
        lv_obj_set_style_bg_color(cell, lv_color_hex(kPieceColors[tc_next_id()]), 0);
    int8_t min_x = 7, min_y = 7;
    for (int i = 0; i < 4; ++i) {
        if (cells[i][0] < min_x) min_x = cells[i][0];
        if (cells[i][1] < min_y) min_y = cells[i][1];
    }
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_pos(s_next_blk[i], s_panel_x + 12 + (cells[i][0] - min_x) * s_cell,
                       58 + (cells[i][1] - min_y) * s_cell);
    }
}

void sync_labels() {
    char text[16];
    if (tc_score() != s_score_cache) {
        s_score_cache = tc_score();
        std::snprintf(text, sizeof(text), "%d", s_score_cache);
        lv_label_set_text(s_score_lbl, text);
        lv_label_set_text(s_val_score, text);
    }
    if (tc_lines() != s_lines_cache) {
        s_lines_cache = tc_lines();
        std::snprintf(text, sizeof(text), "%d", s_lines_cache);
        lv_label_set_text(s_val_lines, text);
        std::snprintf(text, sizeof(text), "%d", tc_level());
        lv_label_set_text(s_val_level, text);
    }
}

void load_best(int *best) {
    *best = 0;
    nvs_handle_t h;
    if (nvs_open("tetris", NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0;
        /* 2026-09-13 修复:原来用 u16,分数 >65535 会静默回绕成小分。
           先按新格式 u32 读;读不到再试旧 u16 键,老存档不丢。 */
        if (nvs_get_u32(h, "best", &v) == ESP_OK) {
            *best = (int)v;
        } else {
            uint16_t old = 0;
            if (nvs_get_u16(h, "best", &old) == ESP_OK) *best = old;
        }
        nvs_close(h);
    }
}

void save_best(int best) {
    nvs_handle_t h;
    if (nvs_open("tetris", NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u32(h, "best", (uint32_t)best) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void enter_ready() {
    tc_reset();
    s_score_cache = -1;
    s_lines_cache = -1;
    s_gravity_acc = 0;
    sync_labels();
    render_all();
    if (s_dead_panel) lv_obj_set_y(s_dead_panel, 344); /* 移出屏幕 */
    if (s_controls) lv_obj_remove_flag(s_controls, LV_OBJ_FLAG_HIDDEN);
    if (s_hint_lbl) lv_label_set_text(s_hint_lbl, "确定开始");
}

void on_dead() {
    const int score = tc_score();
    if (score > s_best) {
        s_best = score;
        save_best(s_best);
    }
    char text[48];
    std::snprintf(text, sizeof(text), "%d", score);
    lv_label_set_text(s_dead_info, text);
    std::snprintf(text, sizeof(text), "消行 %d   最高 %d", tc_lines(), s_best);
    lv_label_set_text(s_dead_best, text);
    sync_labels();
    if (s_dead_panel) lv_obj_set_y(s_dead_panel, 36 + (s_field_h - 112) / 2); /* 居中盖住棋盘 */
    if (s_hint_lbl) lv_label_set_text(s_hint_lbl, "确定再来一局");
    ESP_LOGI(kTag, "game over: score=%d lines=%d best=%d free=%u",
             score, tc_lines(), s_best, (unsigned)esp_get_free_heap_size());
}

void build_ui(); /* 定义在 tick 之后;首帧由 tick 在 LVGL 任务里调用 */

void tick(lv_timer_t *) {
    if (!s_active.load()) return;
    ++s_frame;

    /* 首帧建 UI:建 200 个格子对象的 LVGL 调用链在 LVGL 任务 7KB 栈上跑;
       输入任务只有 3KB,直接建会栈保护崩溃(真机踩过)。 */
    if (!s_screen) {
        build_ui();
        lv_screen_load(s_screen);
        return;
    }

    const bool visible = app_registry_screen_is_on();
    const bool playing = tc_phase() == TC_PLAYING;
    if (s_timer) lv_timer_set_period(s_timer, visible || playing ? kTickMs : 1000);
    if (!visible && !playing) return;
    const int64_t now = esp_timer_get_time();
    if (now - s_battery_us >= 5000000) { ui_pixel_battery_set(&s_battery, bsp_battery_soc()); s_battery_us = now; }
    if (!s_board) return; /* 内存不足提示页:没有游戏状态可推进 */

    if (tc_phase() == TC_PLAYING) {
        app_registry_keep_awake(); /* 下落不依赖按键,进行中必须抑制息屏 */
        int32_t interval = 800 - (tc_level() - 1) * 70;
        if (interval < 120) interval = 120;
        s_gravity_acc += (int32_t)kTickMs;
        if (s_gravity_acc >= interval) {
        s_gravity_acc = 0;
        tc_gravity();
        render_all();
        if (tc_phase() == TC_PLAYING) render_next(); /* 锁定出新方块时同步预览 */
        sync_labels();
        if (tc_phase() == TC_DEAD) on_dead();
        }
    }
}

/* 按当前 s_cell 建立棋盘容器 + 格子网格;任一步失败返回 false(调用方降档) */
bool build_board() {
    ensure_styles();
    /* 棋盘底色设为纯黑 0x000000,四周与每格之间 1px 缝隙天然呈现纯黑网格线 */
    s_board = block(s_screen, 12, 30, s_field_w + 1, s_field_h + 1, 0x000000);
    if (!s_board) return false;
    for (int r = 0; r < TC_ROWS; ++r) {
        for (int c = 0; c < TC_COLS; ++c) {
            /* (cell-1) 边长 + 1px 偏移,四周自然形成 1px 纯黑像素网格线 */
            lv_obj_t *o = cell_obj(s_board, 1 + c * s_cell, 1 + r * s_cell, s_cell - 1);
            if (!o) return false;
            s_grid[r][c] = o;
            s_shown[r][c] = 0;
        }
    }
    return true;
}

void drop_board() {
    if (s_board) {
        lv_obj_delete(s_board);
        s_board = nullptr;
    }
    for (int r = 0; r < TC_ROWS; ++r)
        for (int c = 0; c < TC_COLS; ++c) {
            s_grid[r][c] = nullptr;
            s_shown[r][c] = 0;
        }
}

void build_oom_page() {
    s_dead_panel = block(s_screen, 24, 104, 192, 112, UI_PAPER);
    if (!s_dead_panel) return;
    lv_obj_set_style_border_width(s_dead_panel, 1, 0);
    lv_obj_set_style_border_side(s_dead_panel, ui_theme_is_classic()
        ? LV_BORDER_SIDE_FULL
        : static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_border_color(s_dead_panel, lv_color_hex(UI_BORDER), 0);
    s_dead_info = mk_label(s_dead_panel, "内存不足", &buddy_font_16, UI_TEXT,
                           LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_info, 186);
    lv_obj_set_pos(s_dead_info, 3, 30);
    s_dead_best = mk_label(s_dead_panel, "长按确定返回应用", &buddy_font_16, UI_SUB,
                           LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_best, 186);
    lv_obj_set_pos(s_dead_best, 3, 66);
}

void build_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    ui_pixel_background(s_screen);

    lv_obj_t *bar = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = mk_label(bar, "俄罗斯方块", &buddy_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT);
    ui_pixel_status_text_align(title, 8, 84, LV_TEXT_ALIGN_LEFT);
    s_score_lbl = mk_label(bar, "0", &lv_font_montserrat_14, UI_YELLOW, LV_TEXT_ALIGN_RIGHT);
    ui_pixel_status_text_align(s_score_lbl, 96, UI_STATUS_CONTENT_RIGHT - 96,
                               LV_TEXT_ALIGN_RIGHT);
    s_battery = ui_pixel_battery_create(bar, UI_STATUS_BATTERY_X, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc()); s_battery_us = esp_timer_get_time();

    ensure_styles();

    /* 逐档尝试:14(140x280,棋盘几乎铺满屏高)→ 13 → 12 → 10 → 8。
       建不满就删掉重来换小一号,保证不会留下半截棋盘。 */
    const int kCellChoices[] = {12, 10, 8};
    bool ok = false;
    for (int c : kCellChoices) {
        s_cell = c;
        s_field_w = TC_COLS * c;
        s_field_h = TC_ROWS * c;
        s_panel_x = 12 + (s_field_w + 1) + 15;
        if (build_board()) { ok = true; break; }
        ESP_LOGW(kTag, "cell=%d 建格失败(free=%u largest=%u),降一档", c,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        drop_board();
    }
    if (!ok) {
        /* 连最小的格子都建不出来:给出可读提示,别把蓝色过渡屏晾在屏上 */
        ESP_LOGE(kTag, "棋盘对象创建失败(free=%u)", (unsigned)esp_get_free_heap_size());
        build_oom_page();
        return;
    }

    /* 纸色描边框(像素卡片语言),棋盘盖住中间,四周露出 3px。
       边框后建,所以要把它移到棋盘下面去(不能盖住格子)。 */
    lv_obj_t *frame = block(s_screen, 11, 29, (s_field_w + 1) + 2, (s_field_h + 1) + 2, UI_PAPER);
    if (frame) lv_obj_move_to_index(frame, 0);

    lv_obj_t *cap = mk_label(s_screen, "下一个", &buddy_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(cap, s_panel_x, 36);
    for (int i = 0; i < 4; ++i)
        s_next_blk[i] = block(s_screen, s_panel_x, 58, s_cell - 1, s_cell - 1, UI_YELLOW);

    struct Row {
        const char *cap;
        lv_obj_t **val;
        int y;
    };
    /* 四行统计沿棋盘高度均匀铺开;14px 档落点 92/146/200/254。
       cell 变大后右侧只剩 ~76px,"标题+数值并排"会挤爆,
       改成**标题在上、数值在下**的堆叠式,数值独占整列宽。 */
    const Row rows[] = {{"得分", &s_val_score, 107}, {"行数", &s_val_lines, 188}};
    for (const Row &r : rows) {
        auto *caption=mk_label(s_screen,r.cap,&buddy_font_16,UI_TEXT_DIM,LV_TEXT_ALIGN_LEFT);
        lv_obj_set_pos(caption,s_panel_x,r.y);
        *r.val=mk_label(s_screen,"0",&lv_font_montserrat_20,UI_TEXT,LV_TEXT_ALIGN_LEFT);
        lv_obj_set_width(*r.val,240-s_panel_x-12);lv_obj_set_pos(*r.val,s_panel_x,r.y+24);
    }
    // Values remain available to the existing scoring code and game-over panel.
    s_val_level=mk_label(s_screen,"",&lv_font_montserrat_14,UI_TEXT_DIM,LV_TEXT_ALIGN_LEFT);
    s_val_best=mk_label(s_screen,"",&lv_font_montserrat_14,UI_TEXT_DIM,LV_TEXT_ALIGN_LEFT);
    lv_obj_add_flag(s_val_level,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(s_val_best,LV_OBJ_FLAG_HIDDEN);
    s_hint_lbl=ui_pixel_footer(s_screen,"确定开始","长按：下键直落 / 确定返回",&buddy_font_16);
    s_controls=mk_label(s_screen,"上左 · 下右\n确定旋转",&buddy_font_16,UI_TEXT,LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_controls,s_field_w-8);lv_obj_set_pos(s_controls,16,115);
    lv_obj_set_style_bg_color(s_controls,lv_color_hex(UI_BG),0);lv_obj_set_style_bg_opa(s_controls,LV_OPA_COVER,0);
    lv_obj_set_style_pad_ver(s_controls,10,0);lv_obj_set_style_text_line_space(s_controls,6,0);

    s_dead_panel = block(s_screen, 24, 344, 192, 112, UI_PAPER);
    lv_obj_set_style_border_width(s_dead_panel, 1, 0);
    lv_obj_set_style_border_side(s_dead_panel, ui_theme_is_classic()
        ? LV_BORDER_SIDE_FULL
        : static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_set_style_border_color(s_dead_panel, lv_color_hex(UI_BORDER), 0);
    lv_obj_t *t = mk_label(s_dead_panel, "游戏结束", &buddy_font_16, UI_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(t, 186);
    lv_obj_set_pos(t, 3, 12);
    s_dead_info = mk_label(s_dead_panel, "", &lv_font_montserrat_20, UI_SKY_DARK, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_info, 186);
    lv_obj_set_pos(s_dead_info, 3, 40);
    s_dead_best = mk_label(s_dead_panel, "", &buddy_font_16, UI_SUB, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(s_dead_best, 186);
    lv_obj_set_pos(s_dead_best, 3, 74);

    load_best(&s_best);
    char text[16];
    std::snprintf(text, sizeof(text), "%d", s_best);
    lv_label_set_text(s_val_best, text);
    enter_ready();

    ESP_LOGI(kTag, "棋盘就绪 cell=%d (%dx%d); free=%u largest=%u", s_cell, s_field_w,
             s_field_h, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

}  // namespace

extern "C" {

static void tetris_init(void) {}

static void tetris_start(void) {
    if (s_active.load()) return;

    s_active.store(true);
    s_frame = 0;
    bsp_lvgl_lock(-1);
    s_timer = lv_timer_create(tick, kTickMs, nullptr); /* UI 由首帧 tick 在 LVGL 任务里建 */
    bsp_lvgl_unlock();
    ESP_LOGI(kTag, "started; free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void tetris_stop(void) {
    if (!s_active.load()) return;
    s_active.store(false);
    if (s_timer) { /* 先删定时器,防止 tick 撕屏途中再进来 */
        bsp_lvgl_lock(-1);
        lv_timer_del(s_timer);
        s_timer = nullptr;
        bsp_lvgl_unlock();
    }
    /* 2026-09-13 修复(C1):删屏原来写在 if (s_timer) 里面 —— 定时器创建失败
       时整段被跳过,screen 与全部子对象(含 200 个格子)永不释放。
       现在定时器与界面分开,屏幕**无条件**删。 */
    bsp_lvgl_lock(-1);
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    // Shared styles still own heap property arrays. No cell references them
    // after the screen is deleted; retaining them pins holes between sessions.
    if (s_styles_ready) {
        for (auto &style : s_cell_styles) lv_style_reset(&style);
        s_styles_ready = false;
    }
    bsp_lvgl_unlock();

    s_board = nullptr;
    for (int r = 0; r < TC_ROWS; ++r)
        for (int c = 0; c < TC_COLS; ++c) {
            s_grid[r][c] = nullptr;
            s_shown[r][c] = 0;
        }
    s_score_lbl = s_hint_lbl = s_controls = nullptr;
    s_val_score = s_val_lines = s_val_level = s_val_best = nullptr;
    s_dead_panel = s_dead_info = s_dead_best = nullptr;
    for (lv_obj_t *&b : s_next_blk) b = nullptr;
    ESP_LOGI(kTag, "stopped; free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void tetris_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_active.load() || !s_screen) return; /* 首帧 UI 未建前忽略按键 */
    if (!s_board) return; /* 内存不足提示页:只响应长按 OK 的全局返回 */

    /* 本回调跑在输入任务:lv_* 调用必须持锁,否则与渲染任务竞态(真机踩过)。 */
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) lv_timer_set_period(s_timer, kTickMs);

    if (event == BTN_EVT_LONG && btn == BSP_BTN_DOWN && tc_phase() == TC_PLAYING) {
        tc_hard_drop();
        render_all();
        render_next(); /* 硬降锁定后会出新一枚,预览同步 */
        sync_labels();
        if (tc_phase() == TC_DEAD) on_dead();
    } else if (event == BTN_EVT_CLICK) {
        switch (tc_phase()) {
        case TC_READY:
            if (btn == BSP_BTN_OK) {
                if (s_controls) lv_obj_add_flag(s_controls, LV_OBJ_FLAG_HIDDEN);
                s_gravity_acc = 0;
                tc_start((uint32_t)esp_timer_get_time() & 0x7fffffffu);
                render_all();
                render_next();
                sync_labels();
                if (s_hint_lbl) lv_label_set_text(s_hint_lbl, "上左 下右 确定旋转");
            }
            break;
        case TC_PLAYING:
            if (btn == BSP_BTN_UP) {
                tc_move(-1);
                render_all();
            } else if (btn == BSP_BTN_DOWN) {
                tc_move(1);
                render_all();
            } else if (btn == BSP_BTN_OK) {
                tc_rotate();
                render_all();
            }
            break;
        case TC_DEAD:
            if (btn == BSP_BTN_OK) enter_ready();
            break;
        }
    }

    bsp_lvgl_unlock();
}

const passport_app_t g_tetris_app = {
    .id = APP_ID_TETRIS,
    .name = "俄罗斯方块",
    .en_name = "TETRIS",
    .desc = "移动、旋转与消行\n挑战你的最高纪录",
    .tag = "GAME",
    .theme_color = 0x181B1C,
    .init = tetris_init,
    .start = tetris_start,
    .stop = tetris_stop,
    .on_key = tetris_on_key,
};
}
