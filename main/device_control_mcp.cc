#include "device_control.h"
#include "apps/netease/netease_voice_search.h"
#include "apps/radio/radio_catalog.h"
#include <apps/xiaozhi/xiaozhi_ui.h>
#include <cJSON.h>
#include <cstring>
#include <memory>
#include <new>

namespace device_control {
namespace {
const char *const schemas[] = {
R"({"name":"self.pomodoro.control","description":"Control or query the device's actual pomodoro timer (番茄钟). action: status/start/pause/resume/reset. start defaults to focus=25 minutes; short_break=5 and long_break=15 are the only other durations. Do not claim unsupported custom durations. phase is allowed only with start. Starting an already running same phase does not restart it; use resume for a paused timer. Reset only on explicit user request; never automatically reset to resolve a conflict. Starting stays in Xiaozhi and continues counting after leaving it or screen sleep. Open the pomodoro app separately only if asked. Read actual returned status and remaining_seconds. Timer is not saved across power-off. Completion shows on screen; sound waits while another audio app owns the speaker.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["status","start","pause","resume","reset"]},"phase":{"type":"string","enum":["focus","short_break","long_break"]}},"required":["action"],"additionalProperties":false}})",
R"({"name":"self.device.get_status","description":"Read this device's saved speaker volume, screen brightness and current app. Volume and brightness are percentages. Use the actual tool result; do not guess device state.","inputSchema":{"type":"object","properties":{}}})",
R"({"name":"self.audio.set_volume","description":"Change AND persist this device's master speaker volume. Use exactly one of percent (absolute 0..100) or delta (signed percentage points -100..100). For louder/quieter without an amount use delta +10/-10. Zero mutes speech and media; timer and reminder alarms still ring at their fixed alert level. Only claim saved on saved=true; report the returned volume_percent. No external player or server setting is needed.","inputSchema":{"type":"object","properties":{"percent":{"type":"integer","minimum":0,"maximum":100},"delta":{"type":"integer","minimum":-100,"maximum":100}}}})",
R"({"name":"self.screen.set_brightness","description":"Change AND persist this device's screen brightness. Use exactly one of percent (absolute 10..100) or delta (signed percentage points -100..100). For brighter/dimmer without an amount use delta +10/-10. Values clamp to 10..100 to keep the display readable. Only claim saved on saved=true; report brightness_percent. Survives reboot and screen sleep/wake.","inputSchema":{"type":"object","properties":{"percent":{"type":"integer","minimum":10,"maximum":100},"delta":{"type":"integer","minimum":-100,"maximum":100}}}})",
R"({"name":"self.application.open","description":"Open a built-in device app on explicit request. music=网易云音乐; radio=城市收音机; reminders=备忘提醒; lists=文字清单; pomodoro=番茄时钟; settings=设置; home=首页; launcher=应用列表; stopwatch=秒表; tetris=俄罗斯方块; sound_jump=声音跳一跳; game=你在狗叫什么; muse=Muse; voice=随身AI语音; xiaozhi=小智. Ask which app if ambiguous. Opens its screen only. Switching exits this Xiaozhi conversation after the tool reply; do not promise continued background voice control.","inputSchema":{"type":"object","properties":{"app":{"type":"string","enum":["music","radio","reminders","lists","pomodoro","settings","home","launcher","stopwatch","tetris","sound_jump","game","muse","voice","xiaozhi"]}},"required":["app"]}})",
R"({"name":"self.music.search","description":"Search the user's own NetEase Cloud Music (网易云音乐) library on THIS device while staying in this conversation. Call it FIRST for any request naming a song or an artist, and NEVER use search_music or any built-in cloud music tool instead. Returns up to 5 results (song_id, name, artist) in relevance order. If the first result matches the request (title, and singer when given), immediately call self.music.play with source=search, the same query and its song_id, and tell the user which song and singer will play. Only if several versions are equally plausible (original/live/cover) and the user gave no hint, briefly ask which one. NetEase lacks some artists' originals (e.g. 周杰伦): then say only a cover is available and play it if the user agrees.","inputSchema":{"type":"object","properties":{"query":{"type":"string","maxLength":20}},"required":["query"]}})",
R"({"name":"self.music.play","description":"Play music on THIS device from the user's own logged-in NetEase Cloud Music (网易云音乐) account, which has the user's full library and membership. For EVERY request to play a song, an artist or music use this tool (after self.music.search for a song or artist) and NEVER search_music, play_music or any built-in cloud music tool. source=search: the searched query plus song_id of the chosen result; the other results follow. source=favorites: 我喜欢的音乐; daily: 每日推荐; playlist: the user's own or saved playlist, or a chart 热歌榜/新歌榜/飙升榜/原创榜, given as name. shuffle=true for 随机播放. stop_after_minutes pauses playback later, e.g. 半小时后停 = 30. Exits this Xiaozhi conversation after the reply; open Xiaozhi again for more voice control. Except for a chosen search result the playing song is unknown: do not name one. Favorites, daily and personal playlists need the device logged in.","inputSchema":{"type":"object","properties":{"source":{"type":"string","enum":["search","favorites","daily","playlist"]},"query":{"type":"string","maxLength":20},"song_id":{"type":"integer","minimum":1},"name":{"type":"string","maxLength":20},"shuffle":{"type":"boolean"},"stop_after_minutes":{"type":"integer","minimum":1,"maximum":180}},"required":["source"]}})",
R"({"name":"self.radio.play","description":"Play live internet radio for a Chinese city or province in this device's 城市收音机 app, e.g. 打开北京的电台 = city 北京. station: optional keywords of a station name such as 交通 or 音乐; otherwise the most popular local station plays. stop_after_minutes pauses the radio later, e.g. 半小时后停 = 30. Exits this Xiaozhi conversation after the reply. Stations come from a public directory, so do not promise a particular one; the result shows on screen.","inputSchema":{"type":"object","properties":{"city":{"type":"string","maxLength":16},"station":{"type":"string","maxLength":10},"stop_after_minutes":{"type":"integer","minimum":1,"maximum":180}},"required":["city"]}})"
};
struct App {const char *key; passport_app_id_t id;};
constexpr App apps[] = {
    {"music",APP_ID_NETEASE},{"radio",APP_ID_RADIO},{"reminders",APP_ID_REMINDERS},
    {"lists",APP_ID_LISTS},{"pomodoro",APP_ID_POMODORO},{"settings",APP_ID_SETTINGS},{"home",APP_ID_HOME},
    {"launcher",APP_ID_LAUNCHER},{"stopwatch",APP_ID_STOPWATCH},{"tetris",APP_ID_TETRIS},
    {"sound_jump",APP_ID_SOUND_JUMP},{"game",APP_ID_GAME},{"muse",APP_ID_MUSE},
    {"voice",APP_ID_VOICE},{"xiaozhi",APP_ID_XIAOZHI}
};
const char *app_key(passport_app_id_t id) {
    for (const auto &app : apps) if (app.id == id) return app.key;
    return "unknown";
}
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::string print(cJSON *value) {
    char *raw=cJSON_PrintUnformatted(value);
    std::string text=raw?raw:"{}";cJSON_free(raw);return text;
}
std::string reply(cJSON *value, bool error) {
    Json result(cJSON_CreateObject(),cJSON_Delete);
    cJSON *content=cJSON_AddArrayToObject(result.get(),"content");
    cJSON *item=cJSON_CreateObject();cJSON_AddItemToArray(content,item);
    cJSON_AddStringToObject(item,"type","text");
    cJSON_AddStringToObject(item,"text",print(value).c_str());
    cJSON_AddBoolToObject(result.get(),"isError",error);return print(result.get());
}
std::string fail(const char *reason) {
    Json value(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddBoolToObject(value.get(),"saved",false);
    cJSON_AddStringToObject(value.get(),"error",reason);return reply(value.get(),true);
}
bool integer(const cJSON *value,int low,int high) {
    return cJSON_IsNumber(value) && value->valuedouble>=low && value->valuedouble<=high &&
        value->valuedouble==value->valueint;
}
const char *const timer_actions[] = {"status","start","pause","resume","reset"};
const char *const timer_phases[] = {"focus","short_break","long_break"};
const char *const timer_states[] = {"ready","running","paused","finished"};
// Copies a text argument; error names what is missing in words the user hears.
bool text_field(const cJSON *args,const char *key,bool required,char *out,size_t capacity,const char **error) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(args,key);
    const bool city=!std::strcmp(key,"city"),list=!std::strcmp(key,"name"),station=!std::strcmp(key,"station");
    if (!value && !required) return true;
    if (!cJSON_IsString(value) || !value->valuestring[0]) {
        *error=city?"请说出要播放的城市或省份":list?"请说出要播放的歌单名称":station?"电台名称必须是文字":"请说出要播放的歌名或歌手";
        return false;
    }
    if (std::strlen(value->valuestring)>=capacity) {
        *error=city?"城市名称太长":list?"歌单名称太长":station?"电台名称太长":"歌名或歌手太长";
        return false;
    }
    std::strcpy(out,value->valuestring);
    return true;
}
// Runs on the Xiaozhi control task and returns results to the conversation;
// the search itself uses its own short-lived task.
std::string search(const cJSON *args) {
    char query[voice_intent::kNameBytes];
    const char *error=nullptr;
    if (!text_field(args,"query",true,query,sizeof(query),&error)) return fail(error);
    std::unique_ptr<netease::Song[]> songs(new (std::nothrow) netease::Song[netease::kVoiceSearchResults]);
    if (!songs) return fail("设备内存暂时不足，可直接用 self.music.play 的 source=search 播放");
    size_t count=0;
    switch (netease::voice_search(query,songs.get(),&count,8000)) {
        case netease::Result::Ok:case netease::Result::Api:break;  // Api: nothing matched
        case netease::Result::NoMemory:return fail("设备内存暂时不足，可直接用 self.music.play 的 source=search 播放");
        case netease::Result::Cancelled:return fail("搜索超时，可直接用 self.music.play 的 source=search 播放");
        default:return fail("网络错误，搜索失败，请稍后再试");
    }
    Json value(cJSON_CreateObject(),cJSON_Delete);
    cJSON_AddStringToObject(value.get(),"query",query);
    cJSON_AddNumberToObject(value.get(),"found",static_cast<double>(count));
    cJSON *results=cJSON_AddArrayToObject(value.get(),"results");
    for (size_t i=0;i<count;++i) {
        cJSON *item=cJSON_CreateObject();cJSON_AddItemToArray(results,item);
        cJSON_AddNumberToObject(item,"song_id",static_cast<double>(songs[i].id));
        cJSON_AddStringToObject(item,"name",songs[i].name);
        cJSON_AddStringToObject(item,"artist",songs[i].artist);
    }
    return reply(value.get(),false);
}
}
size_t tool_count(){return sizeof(schemas)/sizeof(schemas[0]);}
const char *tool_schema(size_t index){return index<tool_count()?schemas[index]:nullptr;}
std::string call(const char *name,const cJSON *args,uint64_t token) {
    Request request;
    if (!std::strcmp(name,"self.device.get_status")) request.action=Action::Status;
    else if (!std::strcmp(name,"self.audio.set_volume")) request.action=Action::Volume;
    else if (!std::strcmp(name,"self.screen.set_brightness")) request.action=Action::Brightness;
    else if (!std::strcmp(name,"self.application.open")) request.action=Action::Open;
    else if (!std::strcmp(name,"self.pomodoro.control")) request.action=Action::Pomodoro;
    else if (!std::strcmp(name,"self.music.play") || !std::strcmp(name,"self.radio.play")) request.action=Action::Handover;
    else if (!std::strcmp(name,"self.music.search")) return cJSON_IsObject(args)?search(args):fail("请说出要搜索的歌名或歌手");
    else return {};
    if (!cJSON_IsObject(args)) return fail("请提供有效的设备控制参数");
    request.token=token;
    if (request.action==Action::Volume || request.action==Action::Brightness) {
        const cJSON *percent=cJSON_GetObjectItemCaseSensitive(args,"percent");
        const cJSON *delta=cJSON_GetObjectItemCaseSensitive(args,"delta");
        if (bool(percent)==bool(delta)) return fail("请只指定一个绝对百分比或增减幅度");
        const cJSON *value=percent?percent:delta;
        if (!integer(value,delta?-100:request.action==Action::Volume?0:10,100))
            return fail("音量范围0到100，亮度范围10到100；增减幅度为整数百分比");
        request.value=value->valueint;request.relative=delta!=nullptr;
    } else if (request.action==Action::Pomodoro) {
        unsigned fields=0;
        for (const cJSON *field=args->child;field;field=field->next) {
            const unsigned bit=field->string&&!std::strcmp(field->string,"action")?1:
                field->string&&!std::strcmp(field->string,"phase")?2:0;
            if (!bit || (fields&bit)) return fail("番茄钟只接受 action 和 phase，暂不支持自定义时长");
            fields|=bit;
        }
        const auto *action=cJSON_GetObjectItemCaseSensitive(args,"action");
        const auto *phase=cJSON_GetObjectItemCaseSensitive(args,"phase");
        if (!cJSON_IsString(action)) return fail("请指定查询、开始、暂停、继续或重置番茄钟");
        unsigned index=0;
        while (index<5 && std::strcmp(action->valuestring,timer_actions[index])) ++index;
        if (index==5) return fail("不支持的番茄钟操作");
        request.timer_action=static_cast<pomodoro_action_t>(index);
        if (phase) {
            if (request.timer_action!=POMODORO_START || !cJSON_IsString(phase))
                return fail("仅开始计时时可以指定专注或休息类型");
            index=0;
            while (index<3 && std::strcmp(phase->valuestring,timer_phases[index])) ++index;
            if (index==3) return fail("仅支持25分钟专注、5分钟短休息、15分钟长休息");
            request.timer_phase=static_cast<uint8_t>(index);
        }
    } else if (request.action==Action::Open) {
        const auto *app=cJSON_GetObjectItemCaseSensitive(args,"app");
        if (!cJSON_IsString(app)) return fail("请明确要打开哪个应用");
        for (const auto &candidate:apps) if (!std::strcmp(app->valuestring,candidate.key)) request.app=candidate.id;
        if (request.app==APP_ID_COUNT) return fail("设备没有这个应用，请选择内置应用");
    } else if (request.action==Action::Handover) {
        using voice_intent::Source;
        auto &media=request.handover;
        const char *error=nullptr;
        const auto *stop=cJSON_GetObjectItemCaseSensitive(args,"stop_after_minutes");
        if (stop) {
            if (!integer(stop,1,voice_intent::kMaxStopMinutes)) return fail("定时停止请用1到180分钟的整数");
            media.stop_minutes=static_cast<uint16_t>(stop->valueint);
        }
        if (!std::strcmp(name,"self.radio.play")) {
            media.app=APP_ID_RADIO;media.source=Source::Radio;
            if (!text_field(args,"city",true,media.name,sizeof(RadioLocation::city),&error) ||
                !text_field(args,"station",false,media.detail,sizeof(media.detail),&error)) return fail(error);
            if (!radio_catalog_known_place(media.name)) return fail("没有这个城市或省份的电台目录，请说中国的城市或省份名");
        } else {
            media.app=APP_ID_NETEASE;
            const auto *source=cJSON_GetObjectItemCaseSensitive(args,"source");
            const auto *shuffle=cJSON_GetObjectItemCaseSensitive(args,"shuffle");
            const auto *song=cJSON_GetObjectItemCaseSensitive(args,"song_id");
            if (!cJSON_IsString(source)) return fail("请指定搜索歌曲，或播放我喜欢的音乐、每日推荐、某个歌单");
            if (!std::strcmp(source->valuestring,"search")) media.source=Source::Search;
            else if (!std::strcmp(source->valuestring,"favorites")) media.source=Source::Favorites;
            else if (!std::strcmp(source->valuestring,"daily")) media.source=Source::Daily;
            else if (!std::strcmp(source->valuestring,"playlist")) media.source=Source::Playlist;
            else return fail("不支持的音乐来源");
            if (shuffle && !cJSON_IsBool(shuffle)) return fail("随机播放参数必须是 true 或 false");
            media.shuffle=cJSON_IsTrue(shuffle);
            const bool search=media.source==Source::Search;
            if ((search && !text_field(args,"query",true,media.name,sizeof(media.name),&error)) ||
                (media.source==Source::Playlist && !text_field(args,"name",true,media.name,sizeof(media.name),&error)))
                return fail(error);
            if (song) {
                // NetEase ids exceed int; JSON numbers stay exact below 2^53.
                if (!search || !cJSON_IsNumber(song) || song->valuedouble<1 || song->valuedouble>9007199254740991.0 ||
                    song->valuedouble!=static_cast<double>(static_cast<uint64_t>(song->valuedouble)))
                    return fail("song_id 只能配合搜索结果使用，且必须是搜索返回的编号");
                media.song_id=static_cast<uint64_t>(song->valuedouble);
            }
        }
    }
    const Result result=execute(request);
    switch(result.error) {
        case Error::Busy:return fail("设备正在处理其他请求，请稍后再试");
        case Error::Inactive:return fail("小智已退出，未执行设备控制");
        case Error::Storage:return fail("设置保存失败，未应用新值，请稍后重试");
        case Error::Timeout:return fail("执行结果尚未确认，请查询设备状态后再决定是否重试");
        case Error::Alarm:return fail("请先处理当前提醒，再打开其他应用");
        case Error::UnknownApp:return fail("无法打开该应用");
        case Error::AlreadyOpen:return fail("小智已经在运行，无需再次打开");
        case Error::TimerConflict:return fail("番茄钟状态不支持此操作，请先查询状态；已暂停请继续，未开始或已结束请开始。不要自行重置已有计时");
        case Error::TimerInvalid:return fail("无效的番茄钟操作");
        case Error::None:break;
    }
    Json value(cJSON_CreateObject(),cJSON_Delete);
    if (request.action==Action::Pomodoro) {
        cJSON_AddBoolToObject(value.get(),"success",true);
        cJSON_AddStringToObject(value.get(),"phase",timer_phases[result.timer.phase]);
        cJSON_AddStringToObject(value.get(),"status",timer_states[result.timer.status]);
        cJSON_AddNumberToObject(value.get(),"remaining_seconds",result.timer.remaining_seconds);
        cJSON_AddNumberToObject(value.get(),"duration_seconds",pomodoro_duration_us(result.timer.phase)/1000000ULL);
        cJSON_AddNumberToObject(value.get(),"completed_focus_sessions",result.timer.completed);
        cJSON_AddBoolToObject(value.get(),"continues_in_background",true);
        cJSON_AddBoolToObject(value.get(),"persisted",false);
        return reply(value.get(),false);
    }
    cJSON_AddNumberToObject(value.get(),"volume_percent",result.volume);
    cJSON_AddNumberToObject(value.get(),"brightness_percent",result.brightness);
    if (result.applied_brightness<result.brightness) {
        // Saved as asked, shown dimmer until the battery is charged.
        cJSON_AddNumberToObject(value.get(),"applied_brightness_percent",result.applied_brightness);
        cJSON_AddBoolToObject(value.get(),"limited_by_low_battery",true);
    }
    cJSON_AddStringToObject(value.get(),"current_app",app_key(result.app));
    if (request.action==Action::Open || request.action==Action::Handover) {
        const bool handover=request.action==Action::Handover;
        cJSON_AddBoolToObject(value.get(),"accepted",true);
        cJSON_AddStringToObject(value.get(),"target_app",app_key(handover?request.handover.app:request.app));
        cJSON_AddStringToObject(value.get(),"state","switch_after_tool_response");
        if (handover) {
            // The app resolves the request after login or directory lookup; nothing plays yet.
            cJSON_AddBoolToObject(value.get(),"playback_confirmed",false);
            if (request.handover.shuffle) cJSON_AddBoolToObject(value.get(),"shuffle",true);
            if (request.handover.stop_minutes) cJSON_AddNumberToObject(value.get(),"stop_after_minutes",request.handover.stop_minutes);
        }
    } else if (request.action!=Action::Status) {
        cJSON_AddBoolToObject(value.get(),"saved",true);
        if (request.action==Action::Volume) xiaozhi::ui_set_volume(result.volume);
    }
    return reply(value.get(),false);
}
}
