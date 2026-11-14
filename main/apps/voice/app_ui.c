// Device/PC telemetry is authoritative; UP holds the microphone, DOWN sends Enter.
#include "app_ui.h"
#include "app_fonts.h"
#include "audio_streamer.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_screen, *s_mic, *s_elapsed, *s_state, *s_help, *s_channel;
static lv_obj_t *s_notice, *s_keys[4], *s_bars[7], *s_wifi_bars[3];
static ui_pixel_battery_t s_battery;
static lv_obj_t *block(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color) {
    lv_obj_t *o=lv_obj_create(parent); lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_bg_color(o,lv_color_hex(color),0);return o;
}
static lv_obj_t *text(const char *value,const lv_font_t *font,uint32_t color,int x,int y,int w,int h) {
    lv_obj_t *o=ui_pixel_label(s_screen,value,font,color);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);return o;
}
static void hidden(lv_obj_t *o,bool hide) {lv_obj_set_flag(o,LV_OBJ_FLAG_HIDDEN,hide);}
esp_err_t app_ui_init(void) {
    if(s_screen)return ESP_OK;
    s_screen=lv_obj_create(NULL);ui_pixel_background(s_screen);
    lv_obj_t *top=ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, UI_STATUS_WIFI_X, 18, s_wifi_bars);
    lv_obj_t *name=ui_pixel_label(top,"随身语音",&buddy_font_16,UI_TEXT);lv_obj_align(name,LV_ALIGN_LEFT_MID,42,0);
    s_battery=ui_pixel_battery_create(top,UI_STATUS_BATTERY_X,6,UI_TEXT,&lv_font_montserrat_14);
    text("AI语音输入",&ui_font_20,UI_TEXT,12,48,216,28);
    s_mic=lv_obj_create(s_screen);lv_obj_remove_style_all(s_mic);lv_obj_remove_flag(s_mic,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_mic,88,100);lv_obj_set_size(s_mic,64,64);
    block(s_mic,22,0,20,32,UI_ACCENT);
    for(int y=7;y<27;y+=7)block(s_mic,26,y,12,2,UI_BG);
    block(s_mic,14,21,3,20,UI_TEXT_DIM);block(s_mic,47,21,3,20,UI_TEXT_DIM);
    block(s_mic,14,39,36,3,UI_TEXT_DIM);block(s_mic,30,42,4,12,UI_TEXT_DIM);block(s_mic,22,54,20,3,UI_TEXT_DIM);
    for(int i=0;i<7;i++)s_bars[i]=block(s_screen,61+i*18,153,10,2,UI_ACCENT);
    s_elapsed=text("00:00",&ui_digits_32,UI_TEXT,12,157,216,38);
    s_state=text("等待电脑连接",&ui_font_20,UI_WARNING,12,188,216,28);
    s_help=text("启动电脑端转发器",&buddy_font_16,UI_TEXT_DIM,12,220,216,22);
    s_channel=text("电脑输入选择\nCABLE Output\n转发器输出选择\nCABLE Input",&buddy_font_16,UI_TEXT_DIM,12,137,216,112);
    lv_label_set_long_mode(s_channel,LV_LABEL_LONG_WRAP);lv_obj_set_style_text_line_space(s_channel,6,0);
    block(s_screen, 12, 238, 216, 1, UI_BORDER);
    for(int i=0;i<2;i++){
        s_keys[i*2]=text(i?"下键":"上键",&buddy_font_16,UI_TEXT_DIM,34,247+i*25,40,22);
        lv_obj_set_style_border_width(s_keys[i*2],1,0);lv_obj_set_style_border_color(s_keys[i*2],lv_color_hex(UI_BORDER),0);
        lv_obj_set_style_pad_top(s_keys[i*2],2,0);
        s_keys[i*2+1]=text(i?"回车":"按住说话",&buddy_font_16,UI_TEXT,86,247+i*25,120,22);
        lv_obj_set_style_text_align(s_keys[i*2+1],LV_TEXT_ALIGN_LEFT,0);
    }
    s_notice=text("设置后会自动就绪",&buddy_font_16,UI_TEXT_DIM,12,263,216,22);
    text("长按确定返回",&buddy_font_16,UI_TEXT_DIM,12,298,216,20);
    app_ui_snapshot_t snap={.state=APP_ST_READY,.screen_on=true};app_ui_render(&snap);return ESP_OK;
}
void app_ui_show(void){if(s_screen)lv_screen_load(s_screen);}
void app_ui_hide(void){}
void app_ui_deinit(void){if(s_screen)lv_obj_delete(s_screen);s_screen=NULL;memset(s_bars,0,sizeof(s_bars));memset(s_wifi_bars,0,sizeof(s_wifi_bars));s_battery=(ui_pixel_battery_t){0};}
void app_ui_render(const app_ui_snapshot_t *snap){
    if(!s_screen||!snap->screen_on)return;
    const bool ready=!snap->init_failed&&snap->link_up&&snap->pc_ready,channel=!snap->init_failed&&snap->link_up&&!ready;
    const bool recording=ready&&snap->state==APP_ST_LISTENING;
    ui_pixel_battery_set(&s_battery,snap->battery_available?snap->battery_soc:-1);
    const int signal = !snap->wifi_connected ? 0 : snap->wifi_rssi >= -60 ? 3 : snap->wifi_rssi >= -75 ? 2 : 1;
    ui_pixel_wifi_bars_set(s_wifi_bars, signal, snap->wifi_connected ? UI_TEXT : UI_TEXT_DIM);
    hidden(s_mic,recording||channel);hidden(s_elapsed,!recording);hidden(s_channel,!channel);hidden(s_notice,!channel);hidden(s_help,channel);
    for(int i=0;i<4;i++){hidden(s_keys[i],channel);ui_pixel_text_color(s_keys[i],ready?UI_TEXT:UI_TEXT_DIM);}
    const uint16_t peak=recording?audio_streamer_peak():0;
    unsigned height=(unsigned)peak*54/6000;if(height>54)height=54;
    static const unsigned weights[7]={35,60,85,100,85,60,35};
    for(int i=0;i<7;i++){
        hidden(s_bars[i],!recording);unsigned h=height*weights[i]/100;if(h<2)h=2;
        if(lv_obj_get_height(s_bars[i])!=(int)h)lv_obj_set_height(s_bars[i],h);
        if(lv_obj_get_y(s_bars[i])!=155-(int)h)lv_obj_set_y(s_bars[i],155-h);
    }
    char timer[20];unsigned sec=snap->elapsed_ms/1000;snprintf(timer,sizeof(timer),"%02u:%02u",sec/60,sec%60);ui_pixel_label_set_text(s_elapsed,timer);
    lv_obj_set_y(s_state,channel?100:recording?197:188);
    const char *state=snap->init_failed?"语音启动失败":!snap->link_up?"等待电脑连接":channel?"麦克风未就绪":recording?"正在传音":snap->toast[0]?snap->toast:snap->net_busy?"网络不稳":"可以说话了";
    ui_pixel_label_set_text(s_state,state);ui_pixel_text_color(s_state,channel?UI_DANGER:!ready||snap->net_busy?UI_WARNING:UI_TEXT);
    ui_pixel_label_set_text(s_help,snap->init_failed?"长按确定返回后重试":!snap->link_up?(snap->elapsed_ms>=10000?"需与电脑在同一 Wi-Fi":"启动电脑端转发器"):recording?"松开上键结束":snap->net_busy?"靠近路由器后重试":"对着设备麦克风说话");
    ui_pixel_label_set_text(s_keys[1],recording?"松开结束":"按住说话");
}
