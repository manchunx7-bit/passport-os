#include "list_service.h"
#include "apps/app_base.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include <cstdio>
#include <new>

namespace lists {namespace {
struct View {Summary lists[kLists]{};List current{};size_t count=0,selected=0;uint32_t list_id=0,item_id=0,revision=UINT32_MAX;bool deleting=false;};
View *view;
lv_obj_t *screen,*heading,*body,*footer,*lines[3];lv_timer_t *timer;
ui_pixel_battery_t battery{};
const char *message=nullptr;
lv_obj_t *label(const char *text,int x,int y,int w,int h,uint32_t color){
    auto *o=ui_pixel_label(screen,text,&buddy_font_16,color);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP);return o;
}
void refresh(){
    if(!screen)return;
    if(!view){ui_pixel_label_set_text(heading,"内存不足");ui_pixel_label_set_text(body,"请退出后重试");return;}
    auto &v=*view;
    if(v.revision!=revision()){
        const char *error=nullptr;
        if(v.list_id){if(!read(v.list_id,&v.current,&error)){v.list_id=0;v.item_id=0;v.selected=0;message=error;}}
        if(!v.list_id&&!catalog(v.lists,&v.count,&error))message=error;
        v.revision=revision();
    }
    ui_pixel_battery_set(&battery,bsp_battery_soc());
    for(auto *line:lines)lv_obj_add_flag(line,LV_OBJ_FLAG_HIDDEN);
    const size_t count=v.list_id?v.current.count:v.count;
    if(v.selected>=count)v.selected=count?count-1:0;
    if(v.item_id){
        Item *item=nullptr;for(unsigned i=0;i<v.current.count;++i)if(v.current.items[i].id==v.item_id)item=&v.current.items[i];
        if(!item){v.item_id=0;v.deleting=false;}
        else{
            ui_pixel_label_set_text(heading,v.deleting?"删除这项内容？":item->done?"已完成":"待完成");
            ui_pixel_label_set_text(body,item->text);
            ui_pixel_label_set_text(footer,message?message:v.deleting?"确定删除 · 上键取消\n长按确定返回":item->done?"确定改为待完成 · 下键删除\n长按确定返回":"确定标记完成 · 下键删除\n长按确定返回");return;
        }
    }
    if(!count){
        ui_pixel_label_set_text(heading,v.list_id?v.current.title:"还没有清单");
        ui_pixel_label_set_text(body,"对小智说：\n\n帮我整理一份出门清单\n\n也可以添加购物事项");
        ui_pixel_label_set_text(footer,message?message:"确定打开小智\n长按确定返回");return;
    }
    char text[180];std::snprintf(text,sizeof(text),"%s %u/%u",v.list_id?v.current.title:"文字清单",unsigned(v.selected+1),unsigned(count));
    ui_pixel_label_set_text(heading,text);ui_pixel_label_set_text(body,"");
    const size_t start=v.selected/3*3;
    for(size_t i=0;i<3&&start+i<count;++i){
        if(v.list_id){const auto &item=v.current.items[start+i];std::snprintf(text,sizeof(text),"%s %s",item.done?"[x]":"[ ]",item.text);}
        else{const auto &s=v.lists[start+i];std::snprintf(text,sizeof(text),"%s\n已完成 %u / %u",s.title,unsigned(s.done),unsigned(s.count));}
        ui_pixel_label_set_text(lines[i],text);ui_pixel_text_color(lines[i],start+i==v.selected?UI_ON_ACCENT:UI_TEXT);
        ui_pixel_bg_color(lines[i],start+i==v.selected?UI_ACCENT:UI_SURFACE);lv_obj_remove_flag(lines[i],LV_OBJ_FLAG_HIDDEN);
    }
    ui_pixel_label_set_text(footer,message?message:"上下选择 · 确定查看\n长按确定返回");
}
void start(){
    if(!bsp_lvgl_lock(1000))return;
    view=new(std::nothrow) View;message=nullptr;
    screen=lv_obj_create(nullptr);ui_pixel_background(screen);auto *top=ui_pixel_top_bar(screen);
    auto *name=ui_pixel_label(top,"文字清单",&buddy_font_16,UI_TEXT);lv_obj_align(name,LV_ALIGN_LEFT_MID,12,0);
    battery=ui_pixel_battery_create(top,UI_STATUS_BATTERY_X,6,UI_TEXT,&lv_font_montserrat_14);
    heading=label("",12,42,216,26,UI_ACCENT);body=label("",12,80,216,168,UI_TEXT);
    for(unsigned i=0;i<3;++i){lines[i]=label("",12,76+i*60,216,54,UI_TEXT);lv_label_set_long_mode(lines[i],LV_LABEL_LONG_DOT);
        lv_obj_set_style_bg_opa(lines[i],LV_OPA_COVER,0);lv_obj_set_style_pad_all(lines[i],5,0);}
    footer=ui_pixel_footer(screen,"","",&buddy_font_16);lv_obj_set_height(footer,42);lv_label_set_long_mode(footer,LV_LABEL_LONG_WRAP);
    lv_screen_load(screen);refresh();timer=lv_timer_create([](lv_timer_t*){refresh();},1000,nullptr);bsp_lvgl_unlock();
}
void stop(){if(!bsp_lvgl_lock(1000))return;if(timer){lv_timer_delete(timer);timer=nullptr;}if(screen){lv_obj_delete(screen);screen=nullptr;}delete view;view=nullptr;bsp_lvgl_unlock();}
bool back(){if(!bsp_lvgl_lock(500))return true;bool handled=view&&(view->list_id||view->item_id);
    if(handled){if(view->item_id){view->item_id=0;view->deleting=false;}else{view->list_id=0;view->selected=0;}view->revision=UINT32_MAX;message=nullptr;refresh();}bsp_lvgl_unlock();return handled;}
void key(uint8_t btn,app_btn_event_t event,uint16_t){
    if(event!=BTN_EVT_CLICK||!bsp_lvgl_lock(500))return;
    bool voice=false,change=false;uint32_t id=0,version=0,item=0;int done=-1;Action action=Action::Set;
    if(view){auto &v=*view;message=nullptr;
        const size_t count=v.list_id?v.current.count:v.count;
        if(v.item_id){
            if(btn==BSP_BTN_OK){change=true;id=v.list_id;version=v.current.revision;item=v.item_id;action=v.deleting?Action::Remove:Action::Set;
                for(unsigned i=0;i<v.current.count;++i)if(v.current.items[i].id==item)done=!v.current.items[i].done;
            }else if(btn==BSP_BTN_DOWN)v.deleting=true;else if(btn==BSP_BTN_UP)v.deleting=false;
        }else if(!count)voice=btn==BSP_BTN_OK;
        else if(btn==BSP_BTN_OK){if(v.list_id)v.item_id=v.current.items[v.selected].id;
            else{v.list_id=v.lists[v.selected].id;v.selected=0;v.revision=UINT32_MAX;}}
        else if(btn==BSP_BTN_DOWN)v.selected=(v.selected+1)%count;
        else if(btn==BSP_BTN_UP)v.selected=(v.selected+count-1)%count;
    }
    bsp_lvgl_unlock();
    if(voice){app_registry_switch_to(APP_ID_XIAOZHI);return;}
    const char *error=nullptr;const bool changed=change&&modify(id,version,action,item,nullptr,done,nullptr,0,&error);
    if(bsp_lvgl_lock(500)){if(changed&&view){view->item_id=0;view->deleting=false;view->revision=UINT32_MAX;}if(error)message=error;refresh();bsp_lvgl_unlock();}
}
}}
extern const passport_app_t g_lists_app;
const passport_app_t g_lists_app={.id=APP_ID_LISTS,.name="文字清单",.en_name="LISTS",.desc="用小智整理事项\n随时查看完成",.tag="本地文字",.theme_color=0x8CF5A6,
    .init=nullptr,.start=lists::start,.stop=lists::stop,.on_key=lists::key,.prepare_stop=nullptr,.on_back=lists::back};
