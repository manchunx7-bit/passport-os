#pragma once
#include "lvgl.h"

// Tiny draw vocabulary: no image decoding, retained child objects or canvas.
namespace pixel_scene {
struct Ink {
    lv_layer_t *layer;
    lv_area_t origin;
    lv_draw_rect_dsc_t paint;
    explicit Ink(lv_event_t *event) : layer(lv_event_get_layer(event)) {
        lv_obj_get_coords(static_cast<lv_obj_t *>(lv_event_get_target(event)), &origin);
        lv_draw_rect_dsc_init(&paint);
        paint.bg_opa = LV_OPA_COVER;
    }
    // The panel is refreshed in 12-row bands and every band replays the whole
    // scene. Skip shapes outside the band instead of allocating a draw task
    // that LVGL would discard later (the Home scene has ~70 shapes per band).
    bool visible(const lv_area_t &a) const {
        const lv_area_t &clip = layer->_clip_area;
        return a.x2 >= clip.x1 && a.x1 <= clip.x2 && a.y2 >= clip.y1 && a.y1 <= clip.y2;
    }
    void box(int x, int y, int w, int h, uint32_t color) {
        lv_area_t a{origin.x1 + x, origin.y1 + y, origin.x1 + x + w - 1, origin.y1 + y + h - 1};
        if (!visible(a)) return;
        paint.bg_color = lv_color_hex(color);
        lv_draw_rect(layer, &paint, &a);
    }
    void frame(int x, int y, int w, int h, uint32_t color, int t = 1) {
        box(x, y, w, t, color); box(x, y + h - t, w, t, color);
        box(x, y, t, h, color); box(x + w - t, y, t, h, color);
    }
    void rbox(int x, int y, int w, int h, uint32_t fill, int radius,
              uint32_t border = 0, int border_w = 0) {
        lv_area_t a{origin.x1 + x, origin.y1 + y, origin.x1 + x + w - 1, origin.y1 + y + h - 1};
        if (!visible(a)) return;
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(fill);
        d.bg_opa = LV_OPA_COVER;
        d.radius = radius;
        if (border_w > 0) {
            d.border_color = lv_color_hex(border);
            d.border_width = border_w;
            d.border_opa = LV_OPA_COVER;
        }
        lv_draw_rect(layer, &d, &a);
    }
};
} // namespace pixel_scene
