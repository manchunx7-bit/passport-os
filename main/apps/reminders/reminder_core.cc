#include "reminder_core.h"
#include <cstdio>
#include <cstring>
#include <ctime>

namespace reminders {
bool valid_title(const char *text) {
    if (!text) return false;
    const size_t n = strnlen(text, kTitleBytes);
    if (!n || n >= kTitleBytes) return false;
    bool visible = false;
    for (size_t i = 0; i < n;) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 32 || c == 127) return false;
        if (c != ' ') visible = true;
        if (c < 128) { ++i; continue; }
        const unsigned bytes = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!bytes || i + bytes > n) return false;
        for (unsigned j = 1; j < bytes; ++j)
            if ((static_cast<unsigned char>(text[i+j]) & 0xc0) != 0x80) return false;
        const auto b = static_cast<unsigned char>(text[i+1]);
        if ((c == 0xe0 && b < 0xa0) || (c == 0xed && b >= 0xa0) ||
            (c == 0xf0 && b < 0x90) || (c == 0xf4 && b >= 0x90)) return false;
        i += bytes;
    }
    return visible;
}
bool valid_record(const Record &r) {
    return r.id && (r.status == Status::Pending || r.status == Status::Done || r.status == Status::Missed) &&
        r.due >= 1700000000 && r.due < 4102444800LL && valid_title(r.title) && valid_rule(r.repeat);
}
bool valid_rule(const RepeatRule &r) {
    if (!r.days) return !r.local_second && !r.utc_offset_minutes && !r.version;
    return r.days<=127 && r.local_second<86400 && r.utc_offset_minutes>=-720 &&
        r.utc_offset_minutes<=840 && r.version==1;
}
bool make_rule(uint8_t days,int64_t due,int offset,RepeatRule *out) {
    *out={};if(!days)return true;
    if(days>127 || offset%60 || offset < -43200 || offset > 50400 || due<1700000000)return false;
    const int64_t local=due+offset, day=local/86400;
    if(!(days & (1u<<((day+3)%7)))) return false;
    *out={static_cast<uint32_t>(local%86400),static_cast<int16_t>(offset/60),days,1};
    return true;
}
int64_t next_occurrence(const Record &r,int64_t after) {
    if(!r.repeat.days || !valid_rule(r.repeat))return 0;
    const int offset=r.repeat.utc_offset_minutes*60;
    const int64_t today=(after+offset)/86400;
    for(int i=0;i<=7;++i){
        const int64_t day=today+i;
        const int64_t candidate=day*86400+r.repeat.local_second-offset;
        if((r.repeat.days&(1u<<((day+3)%7))) && candidate>after && candidate<4102444800LL)return candidate;
    }return 0;
}
const char *repeat_name(uint8_t days) {
    if(!days)return "一次";
    if(days==127)return "每天";
    if(days==31)return "工作日";
    const char *names[]={"每周一","每周二","每周三","每周四","每周五","每周六","每周日"};
    for(unsigned i=0;i<7;++i)if(days==(1u<<i))return names[i];
    return "重复";
}
bool parse_local(const char *s, int offset, int64_t *utc) {
    if (!s || !utc || offset < -43200 || offset > 50400) return false;
    const size_t n = std::strlen(s);
    if (n != 16 && n != 19) return false;
    if (s[4]!='-' || s[7]!='-' || (s[10]!=' ' && s[10]!='T') || s[13]!=':' || (n==19 && s[16]!=':')) return false;
    for (size_t i=0;i<n;++i) if (i!=4 && i!=7 && i!=10 && i!=13 && i!=16 && (s[i]<'0'||s[i]>'9')) return false;
    const auto two=[&](unsigned i){return (s[i]-'0')*10+s[i+1]-'0';};
    const int year=two(0)*100+two(2), month=two(5), day=two(8), hour=two(11), minute=two(14), second=n==19?two(17):0;
    const auto leap=[](int y){return y%4==0 && (y%100!=0 || y%400==0);};
    const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(year<2024||year>2099||month<1||month>12||day<1||hour>23||minute>59||second>59) return false;
    if(day>days[month-1]+(month==2&&leap(year))) return false;
    int64_t total=0;
    for(int y=1970;y<year;++y) total+=365+leap(y);
    for(int m=1;m<month;++m) total+=days[m-1]+(m==2&&leap(year));
    *utc=(total+day-1)*86400+hour*3600+minute*60+second-offset;
    return true;
}
void format_local(int64_t utc,int offset,char *out,size_t cap) {
    const time_t t=utc+offset; tm value{}; gmtime_r(&t,&value);
    std::strftime(out,cap,"%Y-%m-%d %H:%M:%S",&value);
}
const char *status_text(Status s) {
    return s==Status::Pending?"待提醒":s==Status::Done?"已完成":"已错过";
}
}
