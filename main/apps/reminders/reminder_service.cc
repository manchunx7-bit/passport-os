#include "reminder_service.h"
#include "apps/app_base.h"
#include "launcher/app_registry.h"
#include "apps/voice/time_sync.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "ui_sfx.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace reminders {
namespace {
Record records[kCapacity]{};
StaticSemaphore_t mutex_storage;
SemaphoreHandle_t mutex;
uint32_t next_id=1;
bool storage_ok=false;
std::atomic<uint32_t> rev{0}, active{0};
std::atomic<bool> sound{false};
std::atomic<int> action{0};
int64_t began=0, next_sound=0, next_poll=0, next_timeout_attempt=0;
int64_t active_due=0;
std::atomic<bool> quiet{false};
std::atomic<bool> dismissed{false};
// App interrupted by the alarm; restored after done/snooze/timeout.
passport_app_id_t return_app=APP_ID_COUNT;
const char *alert_error=nullptr;
struct Guard { Guard(){xSemaphoreTake(mutex,portMAX_DELAY);} ~Guard(){xSemaphoreGive(mutex);} };
void key(unsigned index,char (&out)[8]) { std::snprintf(out,sizeof(out),"r%02u",index); }
bool save(unsigned index,const Record &r,uint32_t next) {
    nvs_handle_t handle;
    if(!storage_ok || nvs_open("reminders_v1",NVS_READWRITE,&handle)!=ESP_OK) return false;
    char name[8];key(index,name);
    // Reserve an ID before writing its record. A failed operation may leave
    // a harmless ID gap, but cannot persist a record after ID allocation failed.
    esp_err_t err=nvs_set_u32(handle,"next",next);
    if(err==ESP_OK) err=r.id?nvs_set_blob(handle,name,&r,sizeof(r)):nvs_erase_key(handle,name);
    if(err==ESP_ERR_NVS_NOT_FOUND && !r.id) err=ESP_OK;
    if(err==ESP_OK) err=nvs_commit(handle);
    nvs_close(handle);
    if(err!=ESP_OK) {ESP_LOGW("reminders","Save failed: %s",esp_err_to_name(err));return false;}
    records[index]=r;next_id=next;++rev;return true;
}
void finish_alert() {
    // Return only if the alarm still owns the foreground; a user shortcut
    // or navigation during the alarm keeps the user's choice.
    const auto back=return_app;return_app=APP_ID_COUNT;
    if(back!=APP_ID_COUNT && !dismissed.load() && app_registry_get_active()==APP_ID_REMINDERS) app_registry_switch_to(back);
    active=0;active_due=0;sound=false;action=0;quiet=false;dismissed=false;alert_error=nullptr;
    ui_sfx_cancel_alarm();show_alert(nullptr);
}
}
bool clock_ready(){const auto t=time(nullptr);return t>=1700000000 && t<4102444800LL;}
void initialize() {
    if(mutex) return;
    mutex=xSemaphoreCreateMutexStatic(&mutex_storage);
    nvs_handle_t handle;
    if(nvs_open("reminders_v1",NVS_READWRITE,&handle)!=ESP_OK) return;
    storage_ok=true;
    nvs_get_u32(handle,"next",&next_id);
    for(unsigned i=0;i<kCapacity;++i) {
        Record r{};size_t len=sizeof(r);char name[8];key(i,name);
        if(nvs_get_blob(handle,name,&r,&len)==ESP_OK && (len==sizeof(r)||len==kLegacyRecordBytes) && valid_record(r)) {
            records[i]=r;if(r.id>=next_id) next_id=r.id+1;
        }
    }
    nvs_close(handle);
}
uint32_t revision(){return rev.load();}
size_t list(Record *out,size_t cap) {
    if(!mutex || !out || !cap) return 0;
    Guard g;size_t n=0;
    for(const auto &r:records) if(r.id && n<cap) out[n++]=r;
    std::sort(out,out+n,[](const Record &a,const Record &b){
        if(a.status!=b.status) return static_cast<unsigned>(a.status)<static_cast<unsigned>(b.status);
        return a.due==b.due?a.id<b.id:a.due<b.due;
    });return n;
}
bool get(uint32_t id,Record *out) {
    if(!mutex||!out) return false;
    Guard g;
    for(const auto &r:records) if(id && r.id==id){*out=r;return true;}
    return false;
}
bool create(const char *title,int64_t due,uint32_t *id,const char **error) {
    return create_repeating(title,due,RepeatRule{},id,error);
}
bool create_repeating(const char *title,int64_t due,const RepeatRule &rule,uint32_t *id,const char **error) {
    *error=nullptr;
    if(!mutex||!storage_ok){*error="提醒存储不可用，请重启后重试";return false;}
    if(!clock_ready()){*error="设备尚未校时，请联网后再设置";return false;}
    const int64_t now=time(nullptr);
    if(!valid_title(title)){*error="事项为空、过长或含非法字符，请缩短到约三十个汉字";return false;}
    if(due<=now||due-now>kMaxAhead){*error="时间必须在未来一年以内，请确认日期和时间";return false;}
    if(!valid_rule(rule) || (rule.days && next_occurrence(Record{1,Status::Pending,due,{},rule},due-1)!=due)){
        *error="重复规则与首次提醒时间不一致";return false;
    }
    Guard g;unsigned slot=kCapacity;
    for(unsigned i=0;i<kCapacity;++i) {
        const auto &r=records[i];
        if(!r.id && slot==kCapacity) slot=i;
        if(r.id && r.status==Status::Pending && r.due>=due-2 && r.due<=due+2 && !std::strcmp(r.title,title) &&
            !std::memcmp(&r.repeat,&rule,sizeof(rule))) {*id=r.id;return true;}
    }
    if(slot==kCapacity){*error="已保存二十条，请先删除不需要的提醒";return false;}
    if(!next_id||next_id==UINT32_MAX){*error="提醒编号已用尽";return false;}
    Record r{};r.id=next_id;r.status=Status::Pending;r.due=due;r.repeat=rule;std::strcpy(r.title,title);
    if(!save(slot,r,next_id+1)){*error="保存失败，请重试";return false;}
    *id=r.id;ESP_LOGI("reminders","Saved id=%lu due=%lld",(unsigned long)r.id,(long long)due);return true;
}
bool update(uint32_t id,const char *title,int64_t due,const RepeatRule *rule,const char **error) {
    *error=nullptr;
    if(!mutex||!storage_ok){*error="提醒存储不可用";return false;}
    if(title&&!valid_title(title)){*error="事项内容无效或过长";return false;}
    if((due||rule)&&(!clock_ready()||due<=time(nullptr)||due-time(nullptr)>kMaxAhead)){
        *error="请提供未来一年内的首次提醒时间";return false;
    }
    Guard g;
    for(unsigned i=0;i<kCapacity;++i)if(id&&records[i].id==id){
        Record r=records[i];
        if(title)std::strcpy(r.title,title);
        if(rule)r.repeat=*rule;
        if(due){
            r.due=due;r.status=Status::Pending;
            if(r.repeat.days&&!rule && !make_rule(r.repeat.days,due,r.repeat.utc_offset_minutes*60,&r.repeat)){
                *error="新日期不在原重复规则内，请同时指定新的重复方式";return false;
            }
        }
        if(!valid_record(r)||(r.repeat.days&&due&&next_occurrence(r,due-1)!=due)){
            *error="重复规则与首次提醒时间不一致";return false;
        }
        if(!save(i,r,next_id)){*error="保存失败，请查询当前提醒后再重试";return false;}
        return true;
    }*error="这条提醒已不存在";return false;
}
bool change(uint32_t id,bool erase,int64_t snooze,const char **error,int64_t expected_due) {
    *error=nullptr;if(!mutex){*error="提醒尚未就绪";return false;}
    if(snooze && (!clock_ready()||snooze<=time(nullptr)||snooze-time(nullptr)>kMaxAhead)){*error="设备时间不可用";return false;}
    Guard g;
    for(unsigned i=0;i<kCapacity;++i) if(id && records[i].id==id) {
        Record r=records[i];
        if(expected_due&&r.due!=expected_due){*error="提醒时间已改变，请重新查询；未操作下一次提醒";return false;}
        if(erase) r={};else if(snooze){r.due=snooze;r.status=Status::Pending;}
        else if(r.repeat.days){
            const auto next=next_occurrence(r,std::max<int64_t>(time(nullptr),r.due));
            if(!next){*error="无法计算下一次提醒";return false;}
            r.due=next;r.status=Status::Pending;
        }else r.status=Status::Done;
        if(!save(i,r,next_id)){*error="保存失败，请重试";return false;}
        return true;
    }
    *error="这条提醒已不存在";return false;
}
bool alarm_active(){return active.load()!=0 && !dismissed.load();}
bool sound_pending(){return alarm_active() && sound.load();}
void dismiss_for_navigation() {
    if(!active.load()) return;
    dismissed=true;quiet=true;sound=false;action=0;
    ui_sfx_cancel_alarm();show_alert(nullptr);
}
bool intercept_key(uint8_t btn,int event) {
    if(!alarm_active()) return false;
    if(event==BTN_EVT_CLICK) action=btn==BSP_BTN_OK?1:btn==BSP_BTN_DOWN?2:3;
    return true;
}
void play_music_notice(bool (*cancelled)()) {
    const uint32_t id=active.load();
    if(!id||dismissed.load()||!sound.exchange(false)) return;
    if(bsp_audio_init_feedback()!=ESP_OK){if(active==id&&!quiet.load())sound=true;return;}
    bsp_audio_set_alert_volume(UI_SFX_ALARM_VOLUME);
    bool ok=bsp_audio_set_format(UI_SFX_RATE,16,1)==ESP_OK;
    int16_t pcm[128];const unsigned count=ui_sfx_samples(UI_SFX_ALARM);
    for(unsigned pos=0;ok && pos<count;pos+=128) {
        if(active!=id || quiet.load() || action.load() || (cancelled&&cancelled())) break;
        const unsigned n=std::min(128u,count-pos);
        for(unsigned i=0;i<n;++i) pcm[i]=ui_sfx_sample(UI_SFX_ALARM,pos+i);
        ok=bsp_audio_write(pcm,n*sizeof(int16_t))==ESP_OK;
    }
    vTaskDelay(pdMS_TO_TICKS(60));bsp_audio_deinit();
    ESP_LOGI("reminders","Music notice id=%lu audio_ok=%d",(unsigned long)id,ok);
}
void poll() {
    if(!mutex)return;
    const int64_t mono=esp_timer_get_time();
    Record current{};
    if(active.load()) {
        if(!get(active,&current)||current.status!=Status::Pending||current.due!=active_due){finish_alert();return;}
        const int command=action.exchange(0);
        if(command==1||command==2) {
            const char *err=nullptr;
            if(change(current.id,false,command==2?time(nullptr)+300:0,&err)){finish_alert();return;}
            alert_error=err;
        }else if(command==3){quiet=true;sound=false;ui_sfx_cancel_alarm();}
        if(mono-began>=30000000 && mono>=next_timeout_attempt) {
            next_timeout_attempt=mono+1000000;
            bool saved=false;
            {Guard g;for(unsigned i=0;i<kCapacity;++i)if(records[i].id==current.id){Record r=records[i];
                const auto next=next_occurrence(r,std::max<int64_t>(time(nullptr),r.due));
                if(next){r.due=next;r.status=Status::Pending;}else r.status=Status::Missed;
                saved=save(i,r,next_id);break;}}
            if(saved){finish_alert();return;}quiet=true;sound=false;alert_error="保存失败，请按确定重试";
        }
        // Retry removing the overlay if the UI lock was busy at the gesture.
        if (dismissed.load()) {show_alert(nullptr);return;}
        // Alarms use a fixed level and ring even when master volume is 0.
        if(mono>=next_sound && !quiet) {sound=true;next_sound=mono+5000000;}
        const auto app=app_registry_get_active();
        if((app!=APP_ID_NETEASE && app!=APP_ID_REMINDERS) ||
           (app==APP_ID_NETEASE && sound && mono-began>3000000 && next_sound==began+5000000)) {
            if(return_app==APP_ID_COUNT && app!=APP_ID_LAUNCHER) return_app=app;
            app_registry_switch_to(APP_ID_REMINDERS);
        }
        if(app_registry_get_active()==APP_ID_REMINDERS && sound && ui_sfx_alarm_tick())sound=false;
        app_registry_keep_awake();show_alert(&current,alert_error);
        return;
    }
    if(mono<next_poll || !clock_ready())return;
    next_poll=mono+500000;
    const int64_t now=time(nullptr);
    {Guard g;
        for(unsigned i=0;i<kCapacity;++i) {
            auto r=records[i];if(!r.id||r.status!=Status::Pending||r.due>now)continue;
            // Long overdue entries remain visible as missed, rather than
            // ringing through a backlog after the first post-reboot sync.
            if(now-r.due>300){const auto next=next_occurrence(r,now);
                if(next){r.due=next;r.status=Status::Pending;}else r.status=Status::Missed;
                save(i,r,next_id);continue;}
            if(!current.id || r.due<current.due)current=r;
        }
    }
    if(current.id) {
        active=current.id;active_due=current.due;began=mono;next_timeout_attempt=0;return_app=APP_ID_COUNT;next_sound=mono+5000000;sound=true;quiet=false;dismissed=false;alert_error=nullptr;
        ESP_LOGI("reminders","Due id=%lu",(unsigned long)current.id);
        show_alert(&current);
    }
}
}
