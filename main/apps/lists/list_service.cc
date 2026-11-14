#include "list_service.h"
#include "apps/reminders/reminder_core.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <new>
#include <algorithm>

namespace lists {
namespace {
constexpr uint32_t kMagic=0x3154534c;
StaticSemaphore_t mutex_storage;SemaphoreHandle_t mutex;
std::atomic<uint32_t> rev{0};
using Buffer=std::unique_ptr<List>;
struct Guard {Guard(){xSemaphoreTake(mutex,portMAX_DELAY);}~Guard(){xSemaphoreGive(mutex);}};
struct Storage {nvs_handle_t h{};bool ok;Storage():ok(nvs_open("text_lists_v1",NVS_READWRITE,&h)==ESP_OK){}~Storage(){if(ok)nvs_close(h);}};
bool fail(const char **error,const char *text){*error=text;return false;}
bool text_ok(const char *s,size_t limit){return s&&std::strlen(s)<limit&&reminders::valid_title(s);}
bool valid(const List &r){
    if(r.magic!=kMagic||!r.id||!r.revision||!r.next_item||r.count>kItems||
       !std::memchr(r.title,0,kTitle)||!text_ok(r.title,kTitle))return false;
    for(unsigned i=0;i<r.count;++i){const auto &v=r.items[i];
        if(!v.id||v.id>=r.next_item||v.done>1||!std::memchr(v.text,0,kText)||!text_ok(v.text,kText))return false;
        for(unsigned j=0;j<i;++j)if(r.items[j].id==v.id)return false;
    }return true;
}
void key(unsigned slot,char (&name)[8]){std::snprintf(name,sizeof(name),"list%u",slot);}
bool load(Storage &s,unsigned slot,List *out,const char **error){
    *out={};char name[8];key(slot,name);size_t len=sizeof(*out);
    const auto err=nvs_get_blob(s.h,name,out,&len);
    if(err==ESP_ERR_NVS_NOT_FOUND)return true;
    if(err!=ESP_OK||len!=sizeof(*out)||!valid(*out))return fail(error,"清单存储读取失败，未覆盖原数据");
    return true;
}
bool save(Storage &s,unsigned slot,List &value,const char **error){
    char name[8];key(slot,name);
    auto err=value.id?nvs_set_blob(s.h,name,&value,sizeof(value)):nvs_erase_key(s.h,name);
    if(err==ESP_ERR_NVS_NOT_FOUND&&!value.id)err=ESP_OK;
    if(err==ESP_OK)err=nvs_commit(s.h);
    if(err!=ESP_OK)return fail(error,"清单保存失败，请查询当前内容后再试");
    ++rev;return true;
}
bool append(List &r,const char *const *items,size_t count,const char **error){
    if(!count||count>kBatch||!items)return fail(error,"每次请添加一到三项，可分批添加");
    for(size_t i=0;i<count;++i){
        if(!text_ok(items[i],kText))return fail(error,"事项过长或无效，请缩短到三十个汉字左右");
        bool exists=false;for(unsigned j=0;j<r.count;++j)if(!std::strcmp(r.items[j].text,items[i]))exists=true;
        if(exists)continue;
        if(r.count==kItems)return fail(error,"每份清单最多十二项，请删除不需要的事项");
        if(r.next_item==UINT32_MAX)return fail(error,"事项编号已用尽");
        auto &v=r.items[r.count++];v={};v.id=r.next_item++;std::strcpy(v.text,items[i]);
    }return true;
}
}
void initialize(){if(!mutex)mutex=xSemaphoreCreateMutexStatic(&mutex_storage);}
uint32_t revision(){return rev.load();}
bool catalog(Summary *out,size_t *count,const char **error){
    *error=nullptr;*count=0;if(!mutex)return fail(error,"清单尚未就绪");
    Buffer r(new(std::nothrow) List{});if(!r)return fail(error,"内存不足，请稍后重试");
    Guard g;Storage s;if(!s.ok)return fail(error,"清单存储不可用");
    for(unsigned i=0;i<kLists;++i){if(!load(s,i,r.get(),error))return false;if(!r->id)continue;
        auto &v=out[(*count)++];v={};v.id=r->id;v.count=r->count;std::strcpy(v.title,r->title);
        for(unsigned j=0;j<r->count;++j)v.done+=r->items[j].done;
    }return true;
}
bool read(uint32_t id,List *out,const char **error){
    *error=nullptr;if(!mutex)return fail(error,"清单尚未就绪");
    Guard g;Storage s;if(!s.ok)return fail(error,"清单存储不可用");
    for(unsigned i=0;i<kLists;++i){if(!load(s,i,out,error))return false;if(id&&out->id==id)return true;}
    return fail(error,"这份清单已不存在");
}
bool create(const char *title,const char *const *items,size_t count,uint32_t *id,const char **error){
    *error=nullptr;if(!mutex)return fail(error,"清单尚未就绪");
    if(count>kBatch||(count&&!items))return fail(error,"每次最多三项，请分批保存");
    if(!text_ok(title,kTitle))return fail(error,"清单名称请缩短到十五个汉字左右");
    Buffer r(new(std::nothrow) List{});if(!r)return fail(error,"内存不足，请稍后重试");
    Guard g;Storage s;if(!s.ok)return fail(error,"清单存储不可用");
    unsigned slot=kLists;uint32_t next=1;
    const auto next_err=nvs_get_u32(s.h,"next",&next);
    if(next_err!=ESP_OK&&next_err!=ESP_ERR_NVS_NOT_FOUND)return fail(error,"清单编号读取失败");
    for(unsigned i=0;i<kLists;++i){
        if(!load(s,i,r.get(),error))return false;
        if(!r->id){if(slot==kLists)slot=i;continue;}
        if(!std::strcmp(title,r->title)){
            bool same=count==r->count;
            for(size_t j=0;same&&j<count;++j)same=items&&items[j]&&!std::strcmp(items[j],r->items[j].text);
            if(same){*id=r->id;return true;}
            return fail(error,"已有同名清单，请先读取后追加，不能覆盖原清单");
        }
        if(r->id>=next)next=r->id==UINT32_MAX?0:r->id+1;
    }
    if(slot==kLists)return fail(error,"最多保存五份清单，请先删除不需要的清单");
    if(!next||next==UINT32_MAX)return fail(error,"清单编号已用尽");
    *r={};r->magic=kMagic;r->id=next;r->revision=1;r->next_item=1;std::strcpy(r->title,title);
    if(count&&!append(*r,items,count,error))return false;
    if(nvs_set_u32(s.h,"next",next+1)!=ESP_OK||nvs_commit(s.h)!=ESP_OK)return fail(error,"清单编号保存失败");
    if(!save(s,slot,*r,error))return false;
    *id=next;return true;
}
bool modify(uint32_t id,uint32_t expected,Action action,uint32_t item_id,const char *text,int done,
            const char *const *items,size_t count,const char **error){
    *error=nullptr;if(!mutex)return fail(error,"清单尚未就绪");
    Buffer r(new(std::nothrow) List{});if(!r)return fail(error,"内存不足，请稍后重试");
    Guard g;Storage s;if(!s.ok)return fail(error,"清单存储不可用");
    unsigned slot=0;for(;slot<kLists;++slot){if(!load(s,slot,r.get(),error))return false;if(id&&r->id==id)break;}
    if(slot==kLists)return fail(error,"这份清单已不存在");
    if(!expected||r->revision!=expected)return fail(error,"清单已改变，请重新读取后再操作");
    if(action==Action::Delete){*r={};return save(s,slot,*r,error);}
    if(action==Action::Add){if(!append(*r,items,count,error))return false;}
    else if(action==Action::Rename){
        if(!text_ok(text,kTitle))return fail(error,"清单名称无效或过长");
        // Keep names unique without a second retained list buffer.
        const auto new_title=std::string(text);
        Buffer other(new(std::nothrow) List{});if(!other)return fail(error,"内存不足");
        for(unsigned i=0;i<kLists;++i)if(i!=slot){if(!load(s,i,other.get(),error))return false;
            if(other->id&&!std::strcmp(other->title,new_title.c_str()))return fail(error,"已有同名清单");}
        std::strcpy(r->title,new_title.c_str());
    }else{
        unsigned index=0;while(index<r->count&&r->items[index].id!=item_id)++index;
        if(index==r->count)return fail(error,"这项内容已不存在，请重新读取清单");
        if(action==Action::Remove){
            for(unsigned i=index;i+1<r->count;++i)r->items[i]=r->items[i+1];
            r->items[--r->count]={};
        }else if(action==Action::Set){
            if(!text&&done<0)return fail(error,"请指定文字或完成状态");
            if(text){if(!text_ok(text,kText))return fail(error,"事项无效或过长");std::strcpy(r->items[index].text,text);}
            if(done>=0){if(done>1)return fail(error,"完成状态无效");r->items[index].done=done;}
        }else return fail(error,"不支持的清单操作");
    }
    if(r->revision==UINT32_MAX)return fail(error,"清单版本已用尽");
    ++r->revision;return save(s,slot,*r,error);
}
}
