#include "platform.h"
#include "../main/apps/reminders/reminder_service.cc"
#include "../main/apps/reminders/reminder_core.cc"
#include "../main/apps/reminders/reminder_mcp.cc"
namespace aihot {size_t tool_count(){return 0;}const char *tool_schema(size_t){return nullptr;}std::string call(const char*,const cJSON*){return {};}}
namespace lists {size_t tool_count(){return 0;}const char *tool_schema(size_t){return nullptr;}std::string call(const char*,const cJSON*){return {};}}
namespace device_control {size_t tool_count(){return 0;} const char *tool_schema(size_t){return nullptr;} std::string call(const char*,const cJSON*,uint64_t){return {};} }
static passport_app_id_t foreground=APP_ID_HOME;
static uint32_t shown=0;
extern "C" passport_app_id_t app_registry_get_active(){return foreground;}
extern "C" void app_registry_switch_to(passport_app_id_t id){foreground=id;}
extern "C" void app_registry_keep_awake(){}
extern "C" int time_sync_tz_offset_sec(){return 28800;}
namespace reminders {void show_alert(const Record*r,const char*){shown=r?r->id:0;}}
int main(){
    using namespace reminders;
    int64_t due=0;char formatted[32];
    assert(parse_local("2028-02-29 23:59:59",28800,&due));
    format_local(due,28800,formatted,sizeof(formatted));
    assert(!strcmp(formatted,"2028-02-29 23:59:59"));
    for(auto bad:{"2026-02-29 12:00","2026-04-31 12:00","2026-01-01 24:00","2026-01-01 12:60","2026-01-01 12:00x"})assert(!parse_local(bad,28800,&due));
    assert(valid_title("喝水"));assert(!valid_title("   "));
    assert(!valid_title("x\n"));assert(!valid_title("\xed\xa0\x80"));
    assert(!valid_title(std::string(96,'a').c_str()));
    initialize();const char *error=nullptr;uint32_t id=0;
    fail_write=true;assert(!create("not saved",time(nullptr)+60,&id,&error));
    Record all[kCapacity];assert(list(all,kCapacity)==0);
    fail_write=false;assert(create("喝水",time(nullptr)+60,&id,&error));
    const uint32_t first=id;assert(create("喝水",time(nullptr)+60,&id,&error)&&id==first);
    assert(list(all,kCapacity)==1);
    // Simulated reboot reconstructs reminders from persistent records.
    std::memset(records,0,sizeof(records));mutex=nullptr;next_id=1;initialize();
    assert(get(first,&all[0])&&!strcmp(all[0].title,"喝水"));
    assert(create("next",time(nullptr)+120,&id,&error)&&id>first);
    bool rpc_error=false;
    for(int page=0;page<5;++page){
        auto json=mcp_result("tools/list",("{\"cursor\":\""+std::to_string(page)+"\"}").c_str(),&rpc_error);
        assert(!rpc_error);auto j=cJSON_Parse(json.c_str());assert(cJSON_GetArraySize(cJSON_GetObjectItem(j,"tools"))==1);cJSON_Delete(j);
    }
    mcp_result("tools/list","{\"cursor\":\"bad\"}",&rpc_error);assert(rpc_error);
    auto result=mcp_result("tools/call",R"({"name":"self.reminder.create","arguments":{"title":"bad","due_local":"2026-02-30 10:00","delay_seconds":60}})",&rpc_error);
    assert(result.find("\"isError\":true")!=std::string::npos&&list(all,kCapacity)==2);
    // Due reminder wakes/switches the foreground, rings, then snoozes durably.
    {Guard g;auto r=records[0];r.due=time(nullptr)-1;assert(save(0,r,next_id));}
    poll();assert(alarm_active()&&shown==first);poll();assert(rings==1&&foreground==APP_ID_REMINDERS);
    assert(intercept_key(BSP_BTN_DOWN,BTN_EVT_CLICK));poll();assert(!alarm_active()&&foreground==APP_ID_HOME);
    assert(get(first,&all[0])&&all[0].due>=time(nullptr)+299);
    assert(change(first,false,0,&error));assert(get(first,&all[0])&&all[0].status==Status::Done);
    assert(change(first,true,0,&error));assert(!get(first,&all[0]));
    // Master mute does not silence alarms; music keeps the foreground.
    {Guard g;auto r=records[1];r.due=time(nullptr)-1;assert(save(1,r,next_id));}
    foreground=APP_ID_NETEASE; master_volume=0;monotonic_us+=1000000;
    poll();poll();assert(alarm_active()&&sound_pending()&&foreground==APP_ID_NETEASE);
    assert(intercept_key(BSP_BTN_OK,BTN_EVT_CLICK));poll();master_volume=80;
    // A reboot backlog is retained as missed and does not ring.
    {Guard g;auto r=records[1];r.status=Status::Pending;r.due=time(nullptr)-600;assert(save(1,r,next_id));}
    monotonic_us+=1000000;poll();assert(!alarm_active());
    assert(get(id,&all[0])&&all[0].status==Status::Missed);
    for(unsigned i=1;i<kCapacity;++i){auto title="item"+std::to_string(i);assert(create(title.c_str(),time(nullptr)+600+i,&id,&error));}
    assert(!create("full",time(nullptr)+800,&id,&error));
    // Exact weekly/daily arithmetic, leap days, and fixed offset scheduling.
    Record recurring{};recurring.id=1;recurring.status=Status::Pending;strcpy(recurring.title,"repeat");
    assert(parse_local("2028-02-28 09:15",28800,&recurring.due));
    assert(make_rule(127,recurring.due,28800,&recurring.repeat));
    format_local(next_occurrence(recurring,recurring.due),28800,formatted,sizeof(formatted));
    assert(!strcmp(formatted,"2028-02-29 09:15:00"));
    assert(parse_local("2026-11-06 09:00",28800,&recurring.due)); // Friday
    assert(make_rule(31,recurring.due,28800,&recurring.repeat));
    format_local(next_occurrence(recurring,recurring.due),28800,formatted,sizeof(formatted));
    assert(!strcmp(formatted,"2026-11-09 09:00:00"));
    assert(make_rule(16,recurring.due,28800,&recurring.repeat));
    format_local(next_occurrence(recurring,recurring.due),28800,formatted,sizeof(formatted));
    assert(!strcmp(formatted,"2026-11-13 09:00:00"));
    assert(!make_rule(1,recurring.due,28800,&recurring.repeat));
    // A legacy 112-byte blob remains readable; no destructive migration.
    disk.clear();memset(records,0,sizeof(records));mutex=nullptr;next_id=1;finish_alert();
    Record legacy{};legacy.id=7;legacy.status=Status::Pending;legacy.due=time(nullptr)+120;strcpy(legacy.title,"旧提醒");
    nvs_set_blob(1,"r00",&legacy,kLegacyRecordBytes);initialize();assert(get(7,&all[0])&&!all[0].repeat.days);
    int64_t first_due=time(nullptr)+120;RepeatRule rule{};assert(make_rule(127,first_due,28800,&rule));
    assert(create_repeating("每日喝水",first_due,rule,&id,&error));const auto repeated_id=id;
    assert(update(id,"每日喝水两杯",0,nullptr,&error));
    assert(get(id,&all[0])&&!strcmp(all[0].title,"每日喝水两杯")&&all[0].repeat.days==127&&all[0].due==first_due);
    fail_write=true;assert(!update(id,"不能保存",first_due+60,nullptr,&error));fail_write=false;
    assert(get(id,&all[0])&&!strcmp(all[0].title,"每日喝水两杯")&&all[0].due==first_due);
    assert(update(id,nullptr,first_due+60,nullptr,&error));assert(get(id,&all[0]));
    const auto expected=all[0].due;
    assert(change(id,false,0,&error,expected));assert(get(id,&all[0])&&all[0].due==expected+86400);
    assert(!change(id,false,0,&error,expected)); // stale completion cannot skip another day
    memset(records,0,sizeof(records));mutex=nullptr;next_id=1;initialize();
    assert(get(repeated_id,&all[0])&&all[0].repeat.days==127&&all[0].due==expected+86400);
    // Overdue repeats roll forward once rather than alarming a backlog.
    {Guard g;for(unsigned i=0;i<kCapacity;++i)if(records[i].id==repeated_id){auto r=records[i];r.due=time(nullptr)-600;assert(save(i,r,next_id));}}
    monotonic_us+=1000000;poll();assert(!alarm_active());assert(get(repeated_id,&all[0])&&all[0].due>time(nullptr));
    assert(change(repeated_id,true,0,&error));assert(!get(repeated_id,&all[0])&&get(7,&all[0]));
    for(auto bad:{"10","01","-1","999999999999","x"}){mcp_result("tools/list",("{\"cursor\":\""+std::string(bad)+"\"}").c_str(),&rpc_error);assert(rpc_error);}
    auto changed=mcp_result("tools/call",R"({"name":"self.reminder.update","arguments":{"id":7,"title":"修改旧提醒"}})",&rpc_error);
    assert(changed.find("saved\\\":true")!=std::string::npos&&get(7,&all[0])&&!strcmp(all[0].title,"修改旧提醒"));
    for(const char *args:{R"({"id":7,"title":"bad","delay_seconds":30})",R"({"id":7,"title":"bad","title":"duplicate"})"}){
        auto rejected=mcp_result("tools/call",("{\"name\":\"self.reminder.update\",\"arguments\":"+std::string(args)+"}").c_str(),&rpc_error);
        assert(rejected.find("\"isError\":true")!=std::string::npos);
        assert(get(7,&all[0])&&!strcmp(all[0].title,"修改旧提醒"));
    }
    // Dismiss for global navigation: preserve data, never steal focus back,
    // keep the existing timeout/missed/recurrence policy (including failures).
    for(bool repeats:{false,true}) {
        finish_alert();memset(records,0,sizeof(records));
        Record r{};r.id=42;r.status=Status::Pending;r.due=time(nullptr)-1;strcpy(r.title,"导航提醒");
        if(repeats)assert(make_rule(127,r.due,28800,&r.repeat));
        assert(save(0,r,next_id));monotonic_us+=1000000;poll();assert(alarm_active());
        const auto saved_disk=disk;const auto old_revision=revision();
        dismiss_for_navigation();foreground=APP_ID_XIAOZHI;
        assert(!alarm_active()&&!sound_pending()&&!shown&&disk==saved_disk&&revision()==old_revision);
        assert(!intercept_key(BSP_BTN_OK,BTN_EVT_CLICK));
        const auto old_rings=rings;monotonic_us+=10000000;poll();
        assert(foreground==APP_ID_XIAOZHI&&!shown&&rings==old_rings);
        fail_write=true;monotonic_us+=21000000;poll();
        assert(!alarm_active()&&!shown&&foreground==APP_ID_XIAOZHI);
        fail_write=false;monotonic_us+=1100000;poll();assert(get(42,&r));
        assert(repeats?(r.status==Status::Pending&&r.due>time(nullptr)):r.status==Status::Missed);
    }
    puts("Reminders: strict date/UTF-8, persistence, failure, tools, due/snooze/backlog/capacity, shortcut dismissal PASS");
}
