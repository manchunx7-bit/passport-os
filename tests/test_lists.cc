#include "platform.h"
#include "../main/apps/reminders/reminder_core.cc"
#include "../main/apps/lists/list_service.cc"
#include "../main/apps/lists/list_mcp.cc"
static std::string invoke(const char *name,const char *args){auto *j=cJSON_Parse(args);auto r=lists::call(name,j);cJSON_Delete(j);return r;}
int main(){
    using namespace lists;initialize();const char *error=nullptr;uint32_t id=0;
    const char *items[]={"牛奶两盒","鸡蛋十个","雨伞"};
    fail_write=true;assert(!create("购物清单",items,3,&id,&error));fail_write=false;
    Summary summaries[kLists]{};size_t count=0;assert(catalog(summaries,&count,&error)&&count==0);
    assert(create("购物清单",items,3,&id,&error));const auto first=id;
    List r{};assert(read(id,&r,&error)&&r.count==3&&r.revision==1);
    assert(create("购物清单",items,3,&id,&error)&&id==first);
    assert(!create("购物清单",items,1,&id,&error)); // never replace existing content
    const auto item=r.items[0].id;
    assert(modify(id,r.revision,Action::Set,item,nullptr,1,nullptr,0,&error));
    assert(!modify(id,1,Action::Remove,item,nullptr,-1,nullptr,0,&error)); // stale operation
    assert(read(id,&r,&error)&&r.items[0].done==1&&r.count==3);
    const char *append_items[]={"牛奶两盒","充电器"};
    assert(modify(id,r.revision,Action::Add,0,nullptr,-1,append_items,2,&error));
    assert(read(id,&r,&error)&&r.count==4&&r.items[0].done==1);
    const auto before=disk;
    const char *bad[]={"应整体失败","换行\n内容"};
    assert(!modify(id,r.revision,Action::Add,0,nullptr,-1,bad,2,&error)&&disk==before);
    fail_write=true;assert(!modify(id,r.revision,Action::Set,item,"不得保存",0,nullptr,0,&error));fail_write=false;
    assert(disk==before);
    // Reboot reconstructs records from NVS, without a permanent RAM copy.
    mutex=nullptr;initialize();assert(read(first,&r,&error)&&r.items[0].done==1&&!strcmp(r.items[0].text,"牛奶两盒"));
    assert(modify(first,r.revision,Action::Set,item,"牛奶三盒",0,nullptr,0,&error));
    assert(read(first,&r,&error)&&!strcmp(r.items[0].text,"牛奶三盒")&&!r.items[0].done);
    const auto removed=r.items[1].id;
    assert(modify(first,r.revision,Action::Remove,removed,nullptr,-1,nullptr,0,&error));
    assert(read(first,&r,&error)&&r.count==3&&r.items[1].id!=removed);
    while(r.count<kItems){std::string text="extra"+std::to_string(r.count);const char *v[]={text.c_str()};
        assert(modify(first,r.revision,Action::Add,0,nullptr,-1,v,1,&error));assert(read(first,&r,&error));}
    const char *overflow[]={"overflow"};assert(!modify(first,r.revision,Action::Add,0,nullptr,-1,overflow,1,&error));
    for(unsigned i=1;i<kLists;++i){auto title="list"+std::to_string(i);assert(create(title.c_str(),nullptr,0,&id,&error));}
    assert(!create("too many",nullptr,0,&id,&error));
    assert(!modify(first,r.revision,Action::Rename,0,"list1",-1,nullptr,0,&error));
    auto out=invoke("self.list.read",("{\"list_id\":"+std::to_string(first)+",\"offset\":4}").c_str());
    assert(out.find("next_offset")!=std::string::npos&&out.find("extra")!=std::string::npos);
    for(auto args:{R"({"title":"bad","items":[1]})",R"({"title":"bad","items":["1","2","3","4"]})",R"({"title":"bad","audio":"x"})"})
        assert(invoke("self.list.create",args).find("\"isError\":true")!=std::string::npos);
    assert(invoke("self.list.update",R"({"list_id":1,"revision":1,"action":"set","item_id":1,"done":"yes"})").find("\"isError\":true")!=std::string::npos);
    assert(modify(first,r.revision,Action::Delete,0,nullptr,-1,nullptr,0,&error));assert(!read(first,&r,&error));
    assert(create("replacement",nullptr,0,&id,&error)&&id>first);
    // Corrupt storage is reported instead of treated as a free slot.
    disk["list0"][0]^=1;assert(!catalog(summaries,&count,&error));
    puts("Lists: transactional batch validation, failure, reboot, revisions, stable IDs, done/edit/delete, pagination, capacity and corruption PASS");
}
