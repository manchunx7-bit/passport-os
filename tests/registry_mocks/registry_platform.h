#pragma once
#include <cassert>
#include <cstdint>
#include <set>
#include <cstdio>
using SemaphoreHandle_t=int*;
using TickType_t=unsigned;
constexpr int pdTRUE=1, ESP_OK=0, MALLOC_CAP_INTERNAL=1, ESP_PM_NO_LIGHT_SLEEP=1;
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){return new int(1);}
inline bool mock_mutex_available=true, mock_display_lock=true, mock_chime=false;
inline bool mock_ready=true, mock_setup=true;
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t,unsigned){return mock_mutex_available;}
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t){return 1;}
inline int64_t mock_now=1000000;
inline int64_t esp_timer_get_time(){return mock_now;}
inline unsigned mock_free=100000;
inline unsigned esp_get_free_heap_size(){return mock_free;}
inline unsigned esp_get_minimum_free_heap_size(){return 60000;}
inline unsigned mock_largest=40000;
inline unsigned heap_caps_get_largest_free_block(int){return mock_largest;}
constexpr uint8_t BSP_BTN_UP=0,BSP_BTN_DOWN=1,BSP_BTN_OK=2;
inline bool bsp_lvgl_lock(int){return mock_display_lock;}
inline void bsp_lvgl_unlock(){}
inline void bsp_display_power(bool){}
inline void bsp_display_backlight(int){}
using esp_pm_lock_handle_t=int*;
inline int esp_pm_lock_create(int,int,const char*,esp_pm_lock_handle_t *p){*p=new int(0);return 0;}
inline void esp_pm_lock_acquire(esp_pm_lock_handle_t p){++*p;}
inline void esp_pm_lock_release(esp_pm_lock_handle_t p){--*p;assert(*p>=0);}
enum class WifiPowerSaveLevel{LOW_POWER,BALANCED};
class WifiManager{public: static WifiManager& GetInstance(){static WifiManager w;return w;}
 void SetPowerSaveLevel(WifiPowerSaveLevel){} };
struct lv_obj_t{};
struct lv_timer_t{bool paused=false;};
struct lv_display_t{};
struct lv_event_t{int code;lv_display_t *target;};
inline lv_display_t mock_display;
inline lv_timer_t mock_timer;
inline lv_obj_t *mock_active=nullptr;
inline std::set<lv_obj_t*> mock_screens;
constexpr int LV_EVENT_REFR_REQUEST=1,LV_EVENT_SCREEN_LOADED=2,LV_EVENT_FLUSH_START=3,LV_EVENT_REFR_READY=4;
constexpr int LV_OPA_COVER=255, LV_OBJ_FLAG_SCROLLABLE=1,UI_BG=0;
inline lv_obj_t *lv_obj_create(void*){auto *p=new lv_obj_t;mock_screens.insert(p);return p;}
inline void lv_obj_delete(lv_obj_t *p){assert(p!=mock_active);assert(mock_screens.erase(p));delete p;}
inline lv_obj_t *lv_screen_active(){return mock_active;}
inline void lv_screen_load(lv_obj_t *p){mock_active=p;}
inline lv_display_t *lv_display_get_default(){return &mock_display;}
inline lv_timer_t *lv_display_get_refr_timer(lv_display_t*){return &mock_timer;}
inline void lv_timer_pause(lv_timer_t *p){p->paused=true;}
inline void lv_timer_resume(lv_timer_t *p){p->paused=false;}
inline void lv_timer_ready(lv_timer_t*){}
inline void lv_obj_invalidate(lv_obj_t*){}
inline int lv_color_hex(int x){return x;}
inline void lv_obj_set_style_bg_color(lv_obj_t*,int,int){}
inline void lv_obj_set_style_bg_opa(lv_obj_t*,int,int){}
inline void lv_obj_remove_flag(lv_obj_t*,int){}
inline int lv_event_get_code(lv_event_t *e){return e->code;}
inline void *lv_event_get_target(lv_event_t *e){return e->target;}
inline void lv_display_add_event_cb(lv_display_t*,void(*)(lv_event_t*),int,void*){}
// Clean-launch notice drawing.
struct lv_font_t{};
inline lv_font_t buddy_font_16;
constexpr int UI_TEXT=1, LV_TEXT_ALIGN_CENTER=1, LV_ALIGN_CENTER=1;
inline lv_obj_t mock_label;
inline char mock_notice[64];
inline lv_obj_t *ui_pixel_label(lv_obj_t*,const char *text,const lv_font_t*,int){
 std::snprintf(mock_notice,sizeof(mock_notice),"%s",text);return &mock_label;}
inline void lv_obj_set_width(lv_obj_t*,int){}
inline void lv_obj_set_style_text_align(lv_obj_t*,int,int){}
inline void lv_obj_set_style_text_line_space(lv_obj_t*,int,int){}
inline void lv_obj_align(lv_obj_t*,int,int,int){}
inline void vTaskDelay(TickType_t){}
