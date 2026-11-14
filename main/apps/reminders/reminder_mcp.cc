#include "reminder_service.h"
#include "device_control.h"
#include "apps/lists/list_service.h"
#include "apps/aihot/aihot_mcp.h"
#include "apps/voice/time_sync.h"
#include <cJSON.h>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <memory>

namespace reminders {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::string print(cJSON *value) { char *s=cJSON_PrintUnformatted(value);std::string out=s?s:"{}";cJSON_free(s);return out; }
std::string reply(cJSON *value,bool error=false) {
    Json result(cJSON_CreateObject(),cJSON_Delete);
    cJSON *content=cJSON_AddArrayToObject(result.get(),"content");
    cJSON *item=cJSON_CreateObject();cJSON_AddItemToArray(content,item);
    cJSON_AddStringToObject(item,"type","text");
    const std::string text=print(value);cJSON_AddStringToObject(item,"text",text.c_str());
    cJSON_AddBoolToObject(result.get(),"isError",error);return print(result.get());
}
std::string fail(const char *message) {
    Json value(cJSON_CreateObject(),cJSON_Delete);cJSON_AddBoolToObject(value.get(),"saved",false);
    cJSON_AddStringToObject(value.get(),"error",message);return reply(value.get(),true);
}
bool integer(const cJSON *v,int64_t min,int64_t max) {
    return cJSON_IsNumber(v)&&v->valuedouble>=min&&v->valuedouble<=max&&v->valuedouble==static_cast<int64_t>(v->valuedouble);
}
bool fields(const cJSON *args,std::initializer_list<const char *> allowed) {
    for(auto *p=args->child;p;p=p->next){
        bool found=false;
        for(const auto *key:allowed)if(p->string&&!std::strcmp(p->string,key))found=true;
        if(!found)return false;
        for(auto *q=args->child;q!=p;q=q->next)if(!std::strcmp(q->string,p->string))return false;
    }
    return true;
}
bool rule_arg(const cJSON *args,int64_t due,RepeatRule *out) {
    *out={};const auto *repeat=cJSON_GetObjectItemCaseSensitive(args,"repeat");
    const auto *weekday=cJSON_GetObjectItemCaseSensitive(args,"weekday");
    if(!repeat)return !weekday;
    if(!cJSON_IsString(repeat))return false;
    const char *s=repeat->valuestring;uint8_t days=0;
    if(!std::strcmp(s,"once"))days=0;
    else if(!std::strcmp(s,"daily"))days=127;
    else if(!std::strcmp(s,"weekdays"))days=31;
    else if(!std::strcmp(s,"weekly")){
        if(!integer(weekday,1,7))return false;
        days=1u<<(weekday->valueint-1);
    }else return false;
    if(weekday&&std::strcmp(s,"weekly"))return false;
    return make_rule(days,due,time_sync_tz_offset_sec(),out);
}
void record_json(cJSON *v,const Record &r) {
    const int offset=r.repeat.days?r.repeat.utc_offset_minutes*60:time_sync_tz_offset_sec();
    char when[32];format_local(r.due,offset,when,sizeof(when));
    cJSON_AddNumberToObject(v,"id",r.id);cJSON_AddStringToObject(v,"title",r.title);
    cJSON_AddStringToObject(v,"due_local",when);cJSON_AddNumberToObject(v,"due_unix",r.due);
    cJSON_AddStringToObject(v,"status",status_text(r.status));
    cJSON_AddStringToObject(v,"repeat",!r.repeat.days?"once":r.repeat.days==127?"daily":r.repeat.days==31?"weekdays":"weekly");
    cJSON_AddNumberToObject(v,"utc_offset_minutes",offset/60);
    if(r.repeat.days&&r.repeat.days!=127&&r.repeat.days!=31)for(unsigned i=0;i<7;++i)
        if(r.repeat.days==(1u<<i))cJSON_AddNumberToObject(v,"weekday",i+1);
}
const char *const tools[]={
R"({"name":"self.reminder.get_context","description":"Get the physical device's current local date/time, UTC offset and clock readiness. MUST call before resolving tomorrow, tonight, weekdays or absolute dates. Do not guess current time.","inputSchema":{"type":"object","properties":{}}})",
R"({"name":"self.reminder.create","description":"Save a timed reminder after an explicit request. Use exactly one of delay_seconds or due_local. Call get_context before resolving dates. repeat defaults once; daily, weekdays (Mon-Fri, no holiday calendar), weekly require due_local for the FIRST future occurrence matching the rule. weekly also requires weekday 1=Mon..7=Sun. Completion/missed recurring occurrences advance to the next date, keeping one record. Schedule uses the creation UTC offset. Clarify ambiguous dates. Only confirm saved=true and actual returned time/rule. Device must stay powered; no audio stored. Untimed notes belong in checklist tools.","inputSchema":{"type":"object","properties":{"title":{"type":"string","description":"At most 95 UTF-8 bytes, about 30 Chinese characters"},"delay_seconds":{"type":"integer","minimum":5,"maximum":31622400},"due_local":{"type":"string"},"repeat":{"type":"string","enum":["once","daily","weekdays","weekly"]},"weekday":{"type":"integer","minimum":1,"maximum":7}},"required":["title"]}})",
R"({"name":"self.reminder.list","description":"List saved text reminders, five per page. Use before deleting or completing by id. Follow next_offset to read more.","inputSchema":{"type":"object","properties":{"offset":{"type":"integer","minimum":0,"maximum":20}}}})",
R"({"name":"self.reminder.complete","description":"Complete one occurrence after explicit request. Read list first; use id and its due_unix as expected_due. expected_due is REQUIRED for repeating reminders and prevents completing the next occurrence on retry. Repeating reminders advance to next date; one-time reminders become done. Never guess IDs/timestamps.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1},"expected_due":{"type":"integer"}},"required":["id"]}})",
R"({"name":"self.reminder.delete","description":"Delete a reminder only on explicit request. For repeating reminders this cancels the WHOLE recurring schedule, not only today; clarify if ambiguous. Read list for id first.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1}},"required":["id"]}})",
R"({"name":"self.reminder.update","description":"Modify an existing reminder in place; do not delete/recreate. Read list for id and get_context before resolving time. Omitted fields stay unchanged. title edits text. due_local changes next time and the recurring local time; changing repeat or weekly weekday requires both repeat and a matching FIRST future due_local (weekly also weekday). repeat once turns off repetition. No relative delay parameter: resolve to due_local. Ask which reminder if ambiguous; only confirm saved=true.","inputSchema":{"type":"object","properties":{"id":{"type":"integer","minimum":1},"title":{"type":"string"},"due_local":{"type":"string"},"repeat":{"type":"string","enum":["once","daily","weekdays","weekly"]},"weekday":{"type":"integer","minimum":1,"maximum":7}},"required":["id"]}})"
};
}
std::string mcp_result(const char *method,const char *json,bool *rpc_error,uint64_t token) {
    *rpc_error=false;
    if(!std::strcmp(method,"initialize")) return R"({"protocolVersion":"2024-11-05","capabilities":{"tools":{}},"serverInfo":{"name":"passport-reminders","version":"1"}})";
    Json params(cJSON_Parse(json),cJSON_Delete);
    if(!std::strcmp(method,"tools/list")) {
        const unsigned reminder_tools=sizeof(tools)/sizeof(tools[0]);
        const unsigned total_tools=reminder_tools+device_control::tool_count()+lists::tool_count()+aihot::tool_count();
        unsigned page=0;const auto *cursor=cJSON_GetObjectItem(params.get(),"cursor");
        bool valid=!cursor||cJSON_IsString(cursor);
        if(cJSON_IsString(cursor)&&cursor->valuestring[0]) {
            const char *p=cursor->valuestring;if(p[0]=='0'&&p[1])valid=false;
            for(;*p&&valid;++p){if(*p<'0'||*p>'9'||page>=total_tools){valid=false;break;}page=page*10+(*p-'0');}
        }
        if(!valid||page>=total_tools){*rpc_error=true;return R"({"code":-32602,"message":"Invalid cursor"})";}
        const unsigned control_end=reminder_tools+device_control::tool_count();
        const unsigned lists_end=control_end+lists::tool_count();
        const char *schema=page<reminder_tools?tools[page]:page<control_end?device_control::tool_schema(page-reminder_tools):
            page<lists_end?lists::tool_schema(page-control_end):aihot::tool_schema(page-lists_end);
        std::string out="{\"tools\":[";out+=schema;out+="]";
        if(page+1<total_tools)out+=",\"nextCursor\":\""+std::to_string(page+1)+"\"";
        return out+"}";
    }
    if(std::strcmp(method,"tools/call")){*rpc_error=true;return R"({"code":-32601,"message":"Method not found"})";}
    const auto *name=cJSON_GetObjectItem(params.get(),"name");
    const auto *args=cJSON_GetObjectItem(params.get(),"arguments");
    if(!cJSON_IsString(name)||!cJSON_IsObject(args))return fail("Invalid tool arguments");
    const char *tool=name->valuestring;
    const std::string device_reply=device_control::call(tool,args,token);
    if (!device_reply.empty()) return device_reply;
    const std::string list_reply=lists::call(tool,args);if(!list_reply.empty())return list_reply;
    const std::string news_reply=aihot::call(tool,args);if(!news_reply.empty())return news_reply;
    Json value(cJSON_CreateObject(),cJSON_Delete);
    if(!std::strcmp(tool,"self.reminder.get_context")) {
        const bool ready=clock_ready();char now[32]{};
        if(ready)format_local(time(nullptr),time_sync_tz_offset_sec(),now,sizeof(now));
        cJSON_AddBoolToObject(value.get(),"clock_ready",ready);cJSON_AddStringToObject(value.get(),"local_now",now);
        cJSON_AddNumberToObject(value.get(),"utc_offset_minutes",time_sync_tz_offset_sec()/60);
        cJSON_AddNumberToObject(value.get(),"capacity",kCapacity);
    }else if(!std::strcmp(tool,"self.reminder.create")) {
        if(!fields(args,{"title","delay_seconds","due_local","repeat","weekday"}))return fail("存在无效或重复的提醒参数");
        const auto *title=cJSON_GetObjectItem(args,"title");
        const auto *delay=cJSON_GetObjectItem(args,"delay_seconds");
        const auto *local=cJSON_GetObjectItem(args,"due_local");
        if(!cJSON_IsString(title)||bool(delay)==bool(local))return fail("请提供事项和唯一一种时间；时间不明确时先询问用户");
        int64_t due=0;
        if(delay){if(!integer(delay,5,kMaxAhead))return fail("延迟必须为五秒到一年之间的整数秒");due=time(nullptr)+static_cast<int64_t>(delay->valuedouble);}
        else if(!cJSON_IsString(local)||!parse_local(local->valuestring,time_sync_tz_offset_sec(),&due))return fail("日期格式错误，请使用设备本地 YYYY-MM-DD HH:MM");
        RepeatRule rule{};
        if(!rule_arg(args,due,&rule)||(rule.days&&delay))return fail("重复提醒需匹配规则的未来首次日期；每周需指定周几，工作日为周一至周五");
        uint32_t id=0;const char *error=nullptr;
        if(!create_repeating(title->valuestring,due,rule,&id,&error))return fail(error);
        Record saved{};if(!get(id,&saved))return fail("保存结果读取失败，请查询提醒列表");
        cJSON_AddBoolToObject(value.get(),"saved",true);record_json(value.get(),saved);
    }else if(!std::strcmp(tool,"self.reminder.update")) {
        if(!fields(args,{"id","title","due_local","repeat","weekday"}))return fail("存在无效或重复的修改参数");
        const auto *id=cJSON_GetObjectItemCaseSensitive(args,"id"),*title=cJSON_GetObjectItemCaseSensitive(args,"title");
        const auto *local=cJSON_GetObjectItemCaseSensitive(args,"due_local"),*repeat=cJSON_GetObjectItemCaseSensitive(args,"repeat");
        const auto *weekday=cJSON_GetObjectItemCaseSensitive(args,"weekday");
        if(!integer(id,1,UINT32_MAX)||(!title&&!local&&!repeat)||(title&&!cJSON_IsString(title))||(weekday&&!repeat))return fail("请提供有效编号及要修改的内容或时间");
        int64_t due=0;
        if((repeat&&!local)||(local&&(!cJSON_IsString(local)||!parse_local(local->valuestring,time_sync_tz_offset_sec(),&due))))return fail("请提供有效的未来首次提醒日期");
        RepeatRule rule{};if(repeat&&!rule_arg(args,due,&rule))return fail("重复规则与首次提醒日期不一致");
        const char *error=nullptr;const uint32_t number=static_cast<uint32_t>(id->valuedouble);
        if(!update(number,title?title->valuestring:nullptr,due,repeat?&rule:nullptr,&error))return fail(error);
        Record saved{};if(!get(number,&saved))return fail("请查询确认修改结果");
        cJSON_AddBoolToObject(value.get(),"saved",true);record_json(value.get(),saved);
    }else if(!std::strcmp(tool,"self.reminder.list")) {
        const auto *offset=cJSON_GetObjectItem(args,"offset");
        if(offset&&!integer(offset,0,kCapacity))return fail("Invalid list offset");
        // Bounded heap buffer; don't put 2240 B on the voice control stack.
        std::unique_ptr<Record[]> rows(new(std::nothrow) Record[kCapacity]);if(!rows)return fail("内存不足，请稍后重试");
        const size_t n=list(rows.get(),kCapacity),start=offset?static_cast<size_t>(offset->valuedouble):0;
        cJSON_AddNumberToObject(value.get(),"total",n);cJSON *items=cJSON_AddArrayToObject(value.get(),"reminders");
        for(size_t i=start;i<n&&i<start+5;++i) {
            const auto &r=rows[i];
            cJSON *item=cJSON_CreateObject();cJSON_AddItemToArray(items,item);
            record_json(item,r);
        }
        if(start+5<n)cJSON_AddNumberToObject(value.get(),"next_offset",start+5);
    }else if(!std::strcmp(tool,"self.reminder.complete")||!std::strcmp(tool,"self.reminder.delete")) {
        const auto *id=cJSON_GetObjectItem(args,"id");if(!integer(id,1,UINT32_MAX))return fail("Invalid reminder id");
        const char *error=nullptr;
        const uint32_t number=static_cast<uint32_t>(id->valuedouble);Record before{};
        if(!get(number,&before))return fail("这条提醒已不存在");
        const bool erase=!std::strcmp(tool,"self.reminder.delete");
        const auto *expected=cJSON_GetObjectItemCaseSensitive(args,"expected_due");
        if((expected&&!integer(expected,1700000000,4102444800LL))||(!erase&&before.repeat.days&&!expected))return fail("重复提醒请先查询并提供 due_unix 作为 expected_due，避免误完成下一次");
        if(!change(number,erase,0,&error,expected?static_cast<int64_t>(expected->valuedouble):0))return fail(error);
        cJSON_AddBoolToObject(value.get(),"success",true);
        if(!erase){Record after{};if(get(number,&after))record_json(value.get(),after);}
    }else return fail("Unknown reminder tool");
    return reply(value.get());
}
}
