// Included after the production settings functions by test_device_control.py.
#include "../main/device_control.cc"
#include "../main/device_control_mcp.cc"
#include "../main/apps/reminders/reminder_mcp.cc"
#include "../main/apps/reminders/reminder_core.cc"
#include "../main/apps/lists/list_service.cc"
#include "../main/apps/lists/list_mcp.cc"
#include "../main/apps/aihot/aihot_mcp.cc"
#include <functional>
static passport_app_id_t foreground=APP_ID_XIAOZHI;
static bool navigation_pending=false, mock_alarm=false;
static unsigned switches=0, ui_volume=0, theme_refreshes=0;
static bool display_locked=false, display_available=true, screen_on=false;
bool bsp_lvgl_lock(int){if(!display_available)return false;display_locked=true;return true;}
void bsp_lvgl_unlock(){assert(display_locked);display_locked=false;}
extern "C" bool app_registry_wake_for_notice(){assert(display_locked);screen_on=true;return true;}
extern "C" passport_app_id_t app_registry_get_active(){return foreground;}
extern "C" bool app_registry_switch_pending(){return navigation_pending;}
extern "C" const passport_app_t *app_registry_get_by_id(passport_app_id_t id){
    static passport_app_t app{};return id>APP_ID_LAUNCHER&&id<APP_ID_COUNT?&app:nullptr;
}
extern "C" void app_registry_switch_to(passport_app_id_t id){++switches;foreground=id;}
namespace xiaozhi {
void ui_set_volume(unsigned value){ui_volume=value;}
void ui_refresh_theme(){++theme_refreshes;}
}
static home_profile_t mock_profile={"Mach","保持好奇","珠海","广东省珠海市","22.27","113.57","广东省","珠海","CN"};
extern "C" {
void home_profile_get(home_profile_t *out){*out=mock_profile;}
esp_err_t home_profile_set_text(const home_profile_t *in){
    if(fail_write)return ESP_FAIL;
    if(!in||strlen(in->name)>=HOME_NAME_MAX||strlen(in->signature)>=HOME_SIGNATURE_MAX)return ESP_ERR_INVALID_SIZE;
    mock_profile=*in;return ESP_OK;
}
static home_weather_t mock_weather{true,21,3,68,12,45,1700000000};
static home_air_quality_t mock_air{true,194};
bool home_weather_get(home_weather_t *out){if(!mock_weather.valid)return false;*out=mock_weather;return true;}
bool home_air_quality_get(home_air_quality_t *out){if(!mock_air.valid)return false;*out=mock_air;return true;}
const char *home_weather_code_text(uint16_t code){return code==3?"阴":"晴";}
}
namespace audiobook {
static std::vector<ShelfItem> mock_shelf{
    {3,120,false,"三体全集"},
    {50,50,true,"明朝那些事儿"},
};
size_t read_shelf_summary(ShelfItem *out,size_t cap){
    size_t n=std::min(cap,mock_shelf.size());
    for(size_t i=0;i<n;++i)out[i]=mock_shelf[i];
    return n;
}
}
static pomodoro_t voice_timer=POMODORO_INITIALIZER;
bool pomodoro_control(pomodoro_action_t action,uint8_t phase,PomodoroSnapshot *out,pomodoro_command_result_t *result){
    if(!display_available)return false;
    *result=pomodoro_command(&voice_timer,action,phase,monotonic_us);
    *out={pomodoro_remaining_seconds(&voice_timer,monotonic_us),voice_timer.completed,voice_timer.phase,voice_timer.status};
    return true;
}
extern "C" int time_sync_tz_offset_sec(){return 28800;}
namespace reminders {
bool alarm_active(){return mock_alarm;}
bool clock_ready(){return true;}
bool create(const char*,int64_t,uint32_t*,const char**){assert(false);return false;}
bool create_repeating(const char*,int64_t,const RepeatRule&,uint32_t*,const char**){return false;}
bool update(uint32_t,const char*,int64_t,const RepeatRule*,const char**){return false;}
bool get(uint32_t,Record*){return false;}
bool change(uint32_t,bool,int64_t,const char**,int64_t){return false;}
size_t list(Record*,size_t){return 0;}
}
namespace aihot {
static Query last{};
static unsigned fetches=0;
static Result fetch_result=Result::Ok;
Result fetch(const Query &query,std::string *text,uint32_t){
    ++fetches;last=query;text->clear();
    if(fetch_result==Result::Ok)*text="1. 测试热点（3 个信源，11-07 17:16）\n";
    return fetch_result;
}
}
namespace voice_intent {
static Request armed{};
void arm(const Request &request){armed=request;}
void clear(){armed={};}
}
bool radio_catalog_known_place(const char *name){return !strcmp(name,"北京")||!strcmp(name,"广东");}
namespace netease {
static unsigned searches=0;
static Result search_result=Result::Ok;
Result voice_search(const char *,Song *out,size_t *count,uint32_t){
    ++searches;*count=0;
    if(search_result!=Result::Ok)return search_result;
    const Song rows[]={{1901371647,256000,"孤勇者","陈奕迅"},{1985221664,282733,"孤勇者 (Live版)","陈奕迅"}};
    for(const auto &row:rows)out[(*count)++]=row;
    return Result::Ok;
}
}
static std::string news(const char *name,const char *args){
    cJSON *json=cJSON_Parse(args);auto result=aihot::call(name,json);cJSON_Delete(json);return result;
}
static std::string invoke(const char *name,const char *args,uint64_t token=1){
    cJSON *json=cJSON_Parse(args);auto result=device_control::call(name,json,token);cJSON_Delete(json);return result;
}
int main(){
    using namespace device_control;
    mock_delay_hook=poll;
    auto answer=invoke("self.audio.set_volume",R"({"percent":55})");
    assert(answer.find("saved\\\":true")!=std::string::npos);
    assert(s_volume==55&&master_volume==55&&ui_volume==55);
    invoke("self.audio.set_volume",R"({"delta":10})");assert(s_volume==65);
    invoke("self.audio.set_volume",R"({"delta":100})");assert(s_volume==100);
    invoke("self.audio.set_volume",R"({"percent":0})");assert(s_volume==0);
    s_outdoor_until_us=monotonic_us+120000000;
    invoke("self.screen.set_brightness",R"({"percent":35})");assert(s_brightness==35&&master_brightness==35&&s_outdoor_until_us==0);
    s_volume=80;s_brightness=80;settings_bootstrap();assert(s_volume==0&&s_brightness==35);
    invoke("self.screen.set_brightness",R"({"delta":-100})");assert(s_brightness==10);
    for(auto args:{R"({"percent":0})",R"({"percent":40,"delta":10})",R"({"percent":40.5})",R"({"percent":"40"})",R"({})"}){
        auto error=invoke("self.screen.set_brightness",args);assert(error.find("\"isError\":true")!=std::string::npos);assert(s_brightness==10);
    }
    fail_write=true;
    answer=invoke("self.audio.set_volume",R"({"percent":80})");
    assert(answer.find("\"isError\":true")!=std::string::npos&&s_volume==0&&master_volume==0);
    answer=invoke("self.screen.set_brightness",R"({"percent":80})");assert(s_brightness==10&&master_brightness==10);
    fail_write=false;
    // Prepare only: even a valid open cannot retire its controller before ACK.
    answer=invoke("self.application.open",R"({"app":"music"})",42);
    assert(answer.find("accepted\\\":true")!=std::string::npos&&switches==0);
    monotonic_us+=1000000;poll();assert(switches==0);
    response_sent(42,false);poll();monotonic_us+=1000000;poll();assert(switches==0);
    response_sent(43,true);poll();monotonic_us+=1000000;poll();assert(switches==0);
    response_sent(42,true);poll();monotonic_us+=300000;
    display_available=false;poll();assert(switches==0&&!screen_on);
    display_available=true;poll();assert(switches==1&&foreground==APP_ID_NETEASE&&screen_on&&!display_locked);
    poll();assert(switches==1);
    foreground=APP_ID_XIAOZHI;
    // Explicit navigation, an active mock_alarm or session retirement wins over voice.
    invoke("self.application.open",R"({"app":"settings"})",44);
    response_sent(44,true);navigation_pending=true;poll();navigation_pending=false;monotonic_us+=1000000;poll();assert(switches==1);
    invoke("self.application.open",R"({"app":"home"})",45);response_sent(45,true);cancel_session();poll();monotonic_us+=1000000;poll();assert(switches==1);
    mock_alarm=true;answer=invoke("self.application.open",R"({"app":"home"})",46);mock_alarm=false;assert(answer.find("\"isError\":true")!=std::string::npos);
    answer=invoke("self.application.open",R"({"app":"shell"})",47);assert(answer.find("\"isError\":true")!=std::string::npos);
    answer=invoke("self.application.open",R"({"app":"xiaozhi"})",48);assert(answer.find("\"isError\":true")!=std::string::npos);
    // Voice music: the request is armed for the app and survives the switch.
    using voice_intent::Source;using voice_intent::armed;
    for(auto args:{R"({})",R"({"source":"search"})",R"({"source":"search","query":""})",R"({"source":"search","name":"孤勇者"})",
                  R"({"source":"artist","query":"陈奕迅"})",R"({"source":"playlist"})",R"({"source":"playlist","name":""})",
                  R"({"source":"playlist","name":7})",R"({"source":"playlist","name":"一二三四五六七八九十一二三四五六七八九十一二"})",
                  R"({"source":"favorites","stop_after_minutes":0})",R"({"source":"favorites","stop_after_minutes":181})",
                  R"({"source":"favorites","shuffle":"yes"})",R"({"source":"favorites","song_id":5})",
                  R"({"source":"search","query":"孤勇者","song_id":1.5})",R"({"source":"search","query":"孤勇者","song_id":-3})"}){
        answer=invoke("self.music.play",args,49);assert(answer.find("\"isError\":true")!=std::string::npos&&armed.source==Source::None);
    }
    answer=invoke("self.music.play",R"({"source":"playlist","name":"跑步 歌单"})",50);
    assert(answer.find("accepted\\\":true")!=std::string::npos&&answer.find("playback_confirmed\\\":false")!=std::string::npos);
    assert(armed.app==APP_ID_NETEASE&&armed.source==Source::Playlist&&!strcmp(armed.name,"跑步 歌单")&&switches==1);
    response_sent(50,true);poll();monotonic_us+=300000;poll();
    assert(switches==2&&foreground==APP_ID_NETEASE&&armed.source==Source::Playlist);
    foreground=APP_ID_XIAOZHI;
    // A chosen search result, shuffle and a stop timer travel with the request.
    answer=invoke("self.music.play",R"({"source":"search","query":"孤勇者 陈奕迅","song_id":1985221664,"shuffle":true,"stop_after_minutes":30})",57);
    assert(answer.find("accepted\\\":true")!=std::string::npos&&answer.find("stop_after_minutes\\\":30")!=std::string::npos);
    assert(armed.source==Source::Search&&!strcmp(armed.name,"孤勇者 陈奕迅")&&armed.song_id==1985221664ULL&&armed.shuffle&&armed.stop_minutes==30);
    response_sent(57,true);poll();monotonic_us+=300000;poll();
    assert(switches==3&&foreground==APP_ID_NETEASE&&armed.source==Source::Search);
    foreground=APP_ID_XIAOZHI;
    invoke("self.music.play",R"({"source":"favorites","shuffle":true})",58);
    assert(armed.source==Source::Favorites&&armed.shuffle&&!armed.stop_minutes&&!armed.song_id);
    cancel_session();poll();assert(armed.source==Source::None);
    // Radio: only known places; station keywords and stop timer are optional.
    for(auto args:{R"({})",R"({"city":""})",R"({"city":"火星"})",R"({"city":"北京","station":7})",
                  R"({"city":"北京","stop_after_minutes":200})",R"({"city":"北京","station":"一二三四五六七八九十一"})"}){
        answer=invoke("self.radio.play",args,59);assert(answer.find("\"isError\":true")!=std::string::npos&&armed.source==Source::None);
    }
    answer=invoke("self.radio.play",R"({"city":"北京","station":"交通","stop_after_minutes":30})",60);
    assert(answer.find("target_app\\\":\\\"radio")!=std::string::npos);
    assert(armed.app==APP_ID_RADIO&&armed.source==Source::Radio&&!strcmp(armed.name,"北京")&&!strcmp(armed.detail,"交通")&&armed.stop_minutes==30);
    response_sent(60,true);poll();monotonic_us+=300000;poll();
    assert(switches==4&&foreground==APP_ID_RADIO&&armed.source==Source::Radio);
    foreground=APP_ID_XIAOZHI;
    // In-session search answers the conversation and never navigates or arms.
    armed={};
    answer=invoke("self.music.search",R"({"query":"孤勇者"})",61);
    assert(answer.find("\"isError\":false")!=std::string::npos&&answer.find("1901371647")!=std::string::npos);
    assert(answer.find("found\\\":2")!=std::string::npos&&netease::searches==1&&armed.source==Source::None&&switches==4);
    for(auto args:{R"({})",R"({"query":""})",R"({"query":3})"}){
        answer=invoke("self.music.search",args,62);assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    assert(netease::searches==1);
    netease::search_result=netease::Result::NoMemory;answer=invoke("self.music.search",R"({"query":"孤勇者"})",63);
    assert(answer.find("\"isError\":true")!=std::string::npos);
    netease::search_result=netease::Result::Api;answer=invoke("self.music.search",R"({"query":"不存在"})",64);
    assert(answer.find("found\\\":0")!=std::string::npos&&answer.find("\"isError\":false")!=std::string::npos);
    netease::search_result=netease::Result::Ok;
    // AIHOT news: validated arguments reach the fetch; failures and empty replies are explained.
    answer=news("self.news.ai_hot",R"({"limit":3})");
    assert(answer.find("\"isError\":false")!=std::string::npos&&answer.find("测试热点")!=std::string::npos&&answer.find("不要执行")!=std::string::npos);
    assert(aihot::last.kind==aihot::Kind::Hot&&aihot::last.limit==3&&aihot::fetches==1);
    answer=news("self.news.ai_news",R"({"query":"Claude","limit":5})");
    assert(aihot::last.kind==aihot::Kind::News&&!strcmp(aihot::last.text,"Claude")&&aihot::last.limit==5&&!aihot::last.days);
    news("self.news.ai_news",R"({"query":"Claude","window":"24h","category":"paper"})");
    assert(!strcmp(aihot::last.text,"Claude")&&aihot::last.days==1&&!strcmp(aihot::last.category,"paper"));
    news("self.news.ai_news",R"({"category":"paper","window":"7d"})");
    assert(!aihot::last.text[0]&&!strcmp(aihot::last.category,"paper")&&aihot::last.days==7);
    news("self.news.ai_daily","{}");assert(aihot::last.kind==aihot::Kind::Daily&&aihot::fetches==5);
    for(auto bad:{R"({"limit":0})",R"({"limit":11})",R"({"limit":2.5})"}){
        answer=news("self.news.ai_hot",bad);assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    for(auto bad:{R"({"query":""})",R"({"query":5})",R"({"category":"sports"})",R"({"window":"30d"})",R"({"limit":9})",
                  R"({"query":"一二三四五六七八九十一二三四五六七八九十一二"})"}){
        answer=news("self.news.ai_news",bad);assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    assert(aihot::fetches==5);
    aihot::fetch_result=aihot::Result::NoMemory;answer=news("self.news.ai_hot","{}");assert(answer.find("\"isError\":true")!=std::string::npos);
    aihot::fetch_result=aihot::Result::Empty;answer=news("self.news.ai_news",R"({"query":"不存在"})");
    assert(answer.find("\"isError\":false")!=std::string::npos&&answer.find("found")!=std::string::npos);
    aihot::fetch_result=aihot::Result::Ok;
    // A low battery keeps the saved brightness but caps what is shown, and says so.
    mock_soc=5;answer=invoke("self.screen.set_brightness",R"({"percent":90})");
    assert(s_brightness==90&&master_brightness==60&&answer.find("limited_by_low_battery")!=std::string::npos);
    mock_soc=80;answer=invoke("self.screen.set_brightness",R"({"percent":90})");
    assert(master_brightness==90&&answer.find("limited_by_low_battery")==std::string::npos);
    // Dropped before the switch: session end, a manual switch or expiry clears it.
    invoke("self.music.play",R"({"source":"favorites"})",51);assert(armed.source==Source::Favorites);
    cancel_session();poll();assert(armed.source==Source::None);
    invoke("self.music.play",R"({"source":"daily"})",52);response_sent(52,true);
    foreground=APP_ID_HOME;poll();assert(armed.source==Source::None);foreground=APP_ID_XIAOZHI;
    invoke("self.music.play",R"({"source":"daily"})",53);monotonic_us+=31000000;poll();assert(armed.source==Source::None);
    // A replacing plain open drops the earlier music request.
    invoke("self.music.play",R"({"source":"daily"})",54);invoke("self.application.open",R"({"app":"radio"})",55);
    assert(armed.source==Source::None);response_sent(55,true);poll();monotonic_us+=300000;poll();
    assert(switches==5&&foreground==APP_ID_RADIO);foreground=APP_ID_XIAOZHI;
    // UI contention around a due switch never stalls other tool requests.
    invoke("self.application.open",R"({"app":"home"})",56);response_sent(56,true);poll();monotonic_us+=300000;
    display_available=false;answer=invoke("self.device.get_status","{}");
    assert(answer.find("\"isError\":false")!=std::string::npos&&switches==5);
    display_available=true;poll();assert(switches==6);foreground=APP_ID_XIAOZHI;
    // Input-task stall times out without applying the request later.
    mock_delay_hook=nullptr;
    answer=invoke("self.audio.set_volume",R"({"percent":90})");assert(answer.find("\"isError\":true")!=std::string::npos);
    poll();assert(s_volume==0);mock_delay_hook=poll;
    bool rpc_error=false;
    for(unsigned page=0;page<25;++page){
        auto out=reminders::mcp_result("tools/list",("{\"cursor\":\""+std::to_string(page)+"\"}").c_str(),&rpc_error);
        auto *json=cJSON_Parse(out.c_str());assert(!rpc_error&&cJSON_GetArraySize(cJSON_GetObjectItem(json,"tools"))==1);
        assert(out.size()<1800); // each tool page fits the real receive limit
        assert(bool(cJSON_GetObjectItem(json,"nextCursor"))==(page<24));cJSON_Delete(json);
    }
    answer=invoke("self.pomodoro.control",R"({"action":"start"})");
    assert(answer.find("running")!=std::string::npos&&answer.find("1500")!=std::string::npos&&foreground==APP_ID_XIAOZHI);
    const auto deadline=voice_timer.deadline_us;
    invoke("self.pomodoro.control",R"({"action":"start"})");assert(voice_timer.deadline_us==deadline);
    for(auto args:{R"({})",R"({"action":"toggle"})",R"({"action":"start","minutes":40})",
                  R"({"action":"pause","phase":"focus"})",R"({"action":"start","phase":"custom"})",
                  R"({"action":"start","action":"reset"})"}){
        answer=invoke("self.pomodoro.control",args);assert(answer.find("\"isError\":true")!=std::string::npos);
        assert(voice_timer.deadline_us==deadline);
    }
    invoke("self.pomodoro.control",R"({"action":"pause"})");assert(voice_timer.status==POMODORO_PAUSED);
    answer=invoke("self.pomodoro.control",R"({"action":"start"})");assert(answer.find("\"isError\":true")!=std::string::npos);
    invoke("self.pomodoro.control",R"({"action":"resume"})");assert(voice_timer.status==POMODORO_RUNNING);
    display_available=false;
    answer=invoke("self.pomodoro.control",R"({"action":"reset"})");assert(answer.find("\"isError\":true")!=std::string::npos&&voice_timer.status==POMODORO_RUNNING);
    display_available=true;invoke("self.pomodoro.control",R"({"action":"reset"})");assert(voice_timer.status==POMODORO_READY);
    // Podcast handover accepts supported modes, optional stop timer, and waits for reply.
    for (const auto *args : {R"({})", R"({"source":"daily"})", R"({"source":"search"})",
                             R"({"source":"hot","query":"FM"})", R"({"source":"recommended","latest":1})",
                             R"({"source":"hot","stop_after_minutes":0})",
                             R"({"source":"hot","stop_after_minutes":181})"}) {
        answer=invoke("self.podcast.play",args,90);
        assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    answer=invoke("self.podcast.play",R"({"source":"search","query":"声动早咖啡","latest":true,"stop_after_minutes":30})",91);
    assert(answer.find("target_app\\\":\\\"podcast")!=std::string::npos);
    assert(answer.find("playback_confirmed\\\":false")!=std::string::npos);
    assert(answer.find("stop_after_minutes\\\":30")!=std::string::npos);
    assert(armed.app==APP_ID_PODCAST && armed.source==Source::PodcastSearch && armed.podcast_latest &&
           armed.stop_minutes==30 && !strcmp(armed.name,"声动早咖啡"));
    poll(); assert(foreground==APP_ID_XIAOZHI);
    response_sent(91,true);poll();monotonic_us+=300000;poll();assert(foreground==APP_ID_PODCAST);
    foreground=APP_ID_XIAOZHI;
    invoke("self.podcast.play",R"({"source":"hot"})",92);
    assert(armed.source==Source::PodcastHot && !armed.podcast_latest && armed.stop_minutes==0);
    invoke("self.podcast.play",R"({"source":"recommended"})",93);
    assert(armed.source==Source::PodcastRecommended);
    cancel_session();poll();assert(armed.source==Source::None);
    // Audiobook handover supports search, resume, hot and optional bedtime timer.
    for (const auto *args : {R"({})", R"({"source":"recommended"})", R"({"source":"search"})",
                             R"({"source":"hot","query":"三体"})", R"({"source":"resume","stop_after_minutes":0})",
                             R"({"source":"resume","stop_after_minutes":240})"}) {
        answer=invoke("self.audiobook.play",args,94);
        assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    answer=invoke("self.audiobook.play",R"({"source":"search","query":"盗墓笔记","stop_after_minutes":30})",95);
    assert(answer.find("target_app\\\":\\\"audiobook")!=std::string::npos);
    assert(answer.find("playback_confirmed\\\":false")!=std::string::npos);
    assert(armed.app==APP_ID_AUDIOBOOK && armed.source==Source::AudiobookSearch &&
           armed.stop_minutes==30 && !strcmp(armed.name,"盗墓笔记"));
    poll(); assert(foreground==APP_ID_XIAOZHI);
    response_sent(95,true);poll();monotonic_us+=300000;poll();assert(foreground==APP_ID_AUDIOBOOK);
    foreground=APP_ID_XIAOZHI;
    invoke("self.audiobook.play",R"({"source":"resume"})",96);
    assert(armed.app==APP_ID_AUDIOBOOK && armed.source==Source::AudiobookResume && armed.stop_minutes==0);
    invoke("self.audiobook.play",R"({"source":"hot"})",97);
    assert(armed.source==Source::AudiobookHot);
    cancel_session();poll();assert(armed.source==Source::None);
    // Profile update edits name and/or signature while keeping city/coordinates intact.
    for (const auto *bad : {R"({})", R"({"city":"北京"})", R"({"name":123})", R"({"name":"a\nb"})",
                            R"({"name":"一二三四五六七八九十十一"})"}) {
        answer=invoke("self.profile.update",bad,98);
        assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    answer=invoke("self.profile.update",R"({"name":"小智朋友","signature":"天天开心"})",99);
    assert(answer.find("\"isError\":false")!=std::string::npos&&answer.find("saved\\\":true")!=std::string::npos);
    assert(!strcmp(mock_profile.name,"小智朋友")&&!strcmp(mock_profile.signature,"天天开心")&&
           !strcmp(mock_profile.city,"珠海")&&!strcmp(mock_profile.lat,"22.27"));
    invoke("self.profile.update",R"({"signature":""})",100);
    assert(!strcmp(mock_profile.name,"小智朋友")&&!mock_profile.signature[0]);
    fail_write=true;
    answer=invoke("self.profile.update",R"({"name":"失败测试"})",101);
    assert(answer.find("\"isError\":true")!=std::string::npos&&!strcmp(mock_profile.name,"小智朋友"));
    fail_write=false;
    // Settings update persists theme, screen timeout and key sound with live UI refresh.
    for (const auto *bad : {R"({})", R"({"theme":"dark"})", R"({"screen_timeout_seconds":45})",
                            R"({"key_sound":"yes"})", R"({"theme":"studio","extra":1})"}) {
        answer=invoke("self.settings.update",bad,102);
        assert(answer.find("\"isError\":true")!=std::string::npos);
    }
    const unsigned before_refreshes=theme_refreshes;
    answer=invoke("self.settings.update",R"({"theme":"studio","screen_timeout_seconds":120,"key_sound":false})",103);
    assert(answer.find("saved\\\":true")!=std::string::npos&&answer.find("曜石工坊")!=std::string::npos);
    assert(s_theme==UI_THEME_STUDIO&&applied_theme==UI_THEME_STUDIO&&s_timeout==120&&!mock_key_sound&&
           theme_refreshes==before_refreshes+1);
    s_theme=UI_THEME_CLASSIC;s_timeout=60;mock_key_sound=true;settings_bootstrap();
    assert(s_theme==UI_THEME_STUDIO&&s_timeout==120&&!mock_key_sound);
    invoke("self.settings.update",R"({"screen_timeout_seconds":0})",104);assert(s_timeout==0);
    fail_write=true;
    answer=invoke("self.settings.update",R"({"theme":"classic"})",105);
    assert(answer.find("\"isError\":true")!=std::string::npos&&s_theme==UI_THEME_STUDIO);
    fail_write=false;
    // Status returns battery, theme, timeout, key sound, profile and cached weather/PM2.5.
    answer=invoke("self.device.get_status","{}",106);
    assert(answer.find("battery_percent\\\":80")!=std::string::npos&&answer.find("曜石工坊")!=std::string::npos&&
           answer.find("screen_timeout_seconds\\\":0")!=std::string::npos&&
           answer.find("key_sound\\\":false")!=std::string::npos&&
           answer.find("小智朋友")!=std::string::npos&&answer.find("temp_c\\\":21")!=std::string::npos&&
           answer.find("pm25\\\":19.4")!=std::string::npos);
    mock_weather.valid=false;mock_air.valid=false;mock_soc=-1;
    answer=invoke("self.device.get_status","{}",107);
    assert(answer.find("battery_percent")==std::string::npos&&answer.find("weather_cached\\\":false")!=std::string::npos);
    mock_weather.valid=true;mock_air.valid=true;mock_soc=80;
    // Audiobook shelf summary lists books and rejects unexpected arguments.
    answer=invoke("self.audiobook.shelf",R"({"extra":1})",108);
    assert(answer.find("\"isError\":true")!=std::string::npos);
    answer=invoke("self.audiobook.shelf","{}",109);
    assert(answer.find("\"isError\":false")!=std::string::npos&&answer.find("count\\\":2")!=std::string::npos&&
           answer.find("三体全集")!=std::string::npos&&answer.find("明朝那些事儿")!=std::string::npos);
    audiobook::mock_shelf.clear();
    answer=invoke("self.audiobook.shelf","{}",110);
    assert(answer.find("count\\\":0")!=std::string::npos);
    puts("Device control: saved settings, profile/settings updates, shelf query, guarded navigation, voice pomodoro validation/state, timeout, voice music/radio/podcast/audiobook handover, in-session search, battery/weather status, AIHOT news and 25-tool discovery PASS");
}
