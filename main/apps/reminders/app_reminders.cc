#include "reminder_service.h"
#include "apps/app_base.h"
#include "apps/voice/time_sync.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "bsp_audio.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include <cstdio>
#include <new>
#include <ctime>

namespace reminders {
namespace {
lv_obj_t *screen,*heading,*body,*footer,*lines[3],*alert,*alert_title,*alert_time,*alert_hint;
lv_timer_t *timer;
ui_pixel_battery_t battery{};
Record *rows;
size_t count=0,selected=0;
uint32_t detail_id=0;
bool deleting=false;
const char *error_text=nullptr;
lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int w,int h,uint32_t color) {
    auto *o=ui_pixel_label(parent,text,&buddy_font_16,color);
    lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP);
    return o;
}
void refresh() {
    if(!screen)return;
    if(rows)count=list(rows,kCapacity);else count=0;
    if(selected>=count)selected=count?count-1:0;
    ui_pixel_battery_set(&battery,bsp_battery_soc());
    for(auto *line:lines)lv_obj_add_flag(line,LV_OBJ_FLAG_HIDDEN);
    if(detail_id) {
        Record r{};if(!get(detail_id,&r)){detail_id=0;deleting=false;}
        else {
            char when[32];format_local(r.due,r.repeat.days?r.repeat.utc_offset_minutes*60:time_sync_tz_offset_sec(),when,sizeof(when));
            ui_pixel_label_set_text(heading,deleting?(r.repeat.days?"删除整个重复提醒？":"删除这条提醒？"):status_text(r.status));
            char text[200];std::snprintf(text,sizeof(text),"%s\n%s\n\n%s",when,repeat_name(r.repeat.days),r.title);ui_pixel_label_set_text(body,text);
            ui_pixel_label_set_text(footer,error_text?error_text:deleting?"确定删除 · 上键取消\n长按确定返回":r.repeat.days?"确定完成本次 · 下键删除\n长按确定返回列表":"确定标记完成 · 下键删除\n长按确定返回列表");return;
        }
    }
    if(!rows){ui_pixel_label_set_text(heading,"内存不足");ui_pixel_label_set_text(body,"请退出后重试");return;}
    if(!count) {
        ui_pixel_label_set_text(heading,"还没有提醒");
        ui_pixel_label_set_text(body,"对小智说：\n\n十分钟后提醒我喝水\n\n只保存文字和时间");
        ui_pixel_label_set_text(footer,"确定打开小智\n长按确定返回");return;
    }
    char title[48];std::snprintf(title,sizeof(title),"提醒 %u / %u",unsigned(selected+1),unsigned(count));
    ui_pixel_label_set_text(heading,title);ui_pixel_label_set_text(body,"");
    const size_t start=selected/3*3;
    for(size_t i=0;i<3&&start+i<count;++i) {
        const auto &r=rows[start+i];char when[32],text[160];
        format_local(r.due,r.repeat.days?r.repeat.utc_offset_minutes*60:time_sync_tz_offset_sec(),when,sizeof(when));
        std::snprintf(text,sizeof(text),"%s\n%.11s · %s",r.title,when+5,r.repeat.days?repeat_name(r.repeat.days):status_text(r.status));
        lv_label_set_long_mode(lines[i],LV_LABEL_LONG_DOT);ui_pixel_label_set_text(lines[i],text);
        ui_pixel_text_color(lines[i],start+i==selected?UI_ON_ACCENT:UI_TEXT);
        ui_pixel_bg_color(lines[i],start+i==selected?UI_ACCENT:UI_SURFACE);
        lv_obj_remove_flag(lines[i],LV_OBJ_FLAG_HIDDEN);
    }
    ui_pixel_label_set_text(footer,clock_ready()?"上下选择 · 确定查看\n长按确定返回":"尚未校时，请联网\n上下选择 · 确定查看");
}
void start() {
    if(!bsp_lvgl_lock(1000))return;
    selected=0;detail_id=0;deleting=false;error_text=nullptr;
    rows=new(std::nothrow) Record[kCapacity];
    screen=lv_obj_create(nullptr);ui_pixel_background(screen);
    auto *top=ui_pixel_top_bar(screen);
    auto *name=ui_pixel_label(top,"备忘提醒",&buddy_font_16,UI_TEXT);lv_obj_align(name,LV_ALIGN_LEFT_MID,12,0);
    battery=ui_pixel_battery_create(top,UI_STATUS_BATTERY_X,6,UI_TEXT,&lv_font_montserrat_14);
    heading=label(screen,"",12,42,216,24,UI_ACCENT);
    body=label(screen,"",12,82,216,166,UI_TEXT);
    for(int i=0;i<3;++i) {
        lines[i]=label(screen,"",12,76+i*60,216,54,UI_TEXT);
        lv_obj_set_style_bg_opa(lines[i],LV_OPA_COVER,0);lv_obj_set_style_pad_all(lines[i],5,0);
    }
    footer=ui_pixel_footer(screen,"","",&buddy_font_16);
    lv_obj_set_height(footer,42);
    lv_label_set_long_mode(footer,LV_LABEL_LONG_WRAP);
    lv_screen_load(screen);refresh();
    timer=lv_timer_create([](lv_timer_t*){refresh();},1000,nullptr);
    bsp_lvgl_unlock();
}
void stop() {
    if(!bsp_lvgl_lock(1000))return;
    if(timer){lv_timer_delete(timer);timer=nullptr;}
    if(screen){lv_obj_delete(screen);screen=nullptr;}
    delete[] rows;rows=nullptr;bsp_lvgl_unlock();
}
bool back() {
    if (!bsp_lvgl_lock(500)) return true;
    const bool handled = detail_id != 0;
    if (handled) { detail_id=0; deleting=false; error_text=nullptr; refresh(); }
    bsp_lvgl_unlock();
    return handled;
}
void key(uint8_t btn, app_btn_event_t event, uint16_t) {
    if (event != BTN_EVT_CLICK || !bsp_lvgl_lock(500)) return;
    const bool open_voice = !count && !detail_id && btn == BSP_BTN_OK;
    const uint32_t update_id = detail_id && btn == BSP_BTN_OK ? detail_id : 0;
    const bool erase = deleting;
    if (!update_id && detail_id) {
        if (btn == BSP_BTN_DOWN) deleting=true;
        else if (btn == BSP_BTN_UP) deleting=false;
    } else if (!update_id && count) {
        if (btn == BSP_BTN_OK) {detail_id=rows[selected].id; error_text=nullptr;}
        else if (btn == BSP_BTN_DOWN) selected=(selected+1)%count;
        else if (btn == BSP_BTN_UP) selected=(selected+count-1)%count;
    }
    bsp_lvgl_unlock();
    if (open_voice) {app_registry_switch_to(APP_ID_XIAOZHI); return;}
    // Flash work stays on the input task, outside the LVGL lock.
    const char *err=nullptr;
    const bool changed=update_id && change(update_id,erase,0,&err);
    if (bsp_lvgl_lock(500)) {
        if (changed) {detail_id=0; deleting=false; error_text=nullptr;}
        else if (update_id) error_text=err;
        refresh(); bsp_lvgl_unlock();
    }
}
}
void show_alert(const Record *r,const char *error) {
    if(!bsp_lvgl_lock(100))return;
    if(!r){if(alert)lv_obj_delete(alert);alert=nullptr;bsp_lvgl_unlock();return;}
    if(!app_registry_wake_for_notice()){bsp_lvgl_unlock();return;}
    if(!alert) {
        alert=lv_obj_create(lv_layer_top());lv_obj_remove_flag(alert,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(alert,8,45);lv_obj_set_size(alert,224,228);
        lv_obj_set_style_radius(alert,0,0);lv_obj_set_style_pad_all(alert,0,0);
        lv_obj_set_style_border_width(alert,2,0);lv_obj_set_style_border_color(alert,lv_color_hex(UI_ACCENT),0);
        lv_obj_set_style_bg_color(alert,lv_color_hex(UI_BG),0);lv_obj_set_style_bg_opa(alert,LV_OPA_COVER,0);
        label(alert,"提醒时间到了",10,12,200,24,UI_ACCENT);
        alert_time=label(alert,"",10,42,200,24,UI_TEXT_DIM);
        alert_title=label(alert,"",10,76,200,76,UI_TEXT);
        alert_hint=label(alert,"",10,170,200,48,UI_TEXT_DIM);
    }
    char when[32];format_local(r->due,r->repeat.days?r->repeat.utc_offset_minutes*60:time_sync_tz_offset_sec(),when,sizeof(when));
    ui_pixel_label_set_text(alert_time,when+5);ui_pixel_label_set_text(alert_title,r->title);
    ui_pixel_label_set_text(alert_hint,error?error:"确定完成 · 下键延后5分\n上键静音");
    bsp_lvgl_unlock();
}
}
extern const passport_app_t g_reminders_app;
const passport_app_t g_reminders_app={
    .id=APP_ID_REMINDERS,.name="备忘提醒",.en_name="REMINDERS",
    .desc="用小智记下事项\n到点响铃提醒",.tag="本地提醒",.theme_color=0x8CF5A6,
    .init=nullptr,.start=reminders::start,.stop=reminders::stop,.on_key=reminders::key,
    .prepare_stop=nullptr,.on_back=reminders::back
};
