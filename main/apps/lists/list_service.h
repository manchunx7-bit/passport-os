#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
struct cJSON;
namespace lists {
constexpr unsigned kLists=5,kItems=12,kTitle=48,kText=96,kBatch=3;
struct Item {uint32_t id;uint32_t done;char text[kText];};
struct List {uint32_t magic,id,revision,next_item,count;char title[kTitle];Item items[kItems];};
struct Summary {uint32_t id,count,done;char title[kTitle];};
enum class Action {Add,Set,Remove,Rename,Delete};
void initialize();
uint32_t revision();
bool catalog(Summary *out,size_t *count,const char **error);
bool read(uint32_t id,List *out,const char **error);
bool create(const char *title,const char *const *items,size_t count,uint32_t *id,const char **error);
// expected revision prevents a stale delete/edit from changing a newer list.
bool modify(uint32_t id,uint32_t expected,Action action,uint32_t item_id,
            const char *text,int done,const char *const *items,size_t count,const char **error);
size_t tool_count();
const char *tool_schema(size_t index);
std::string call(const char *name,const cJSON *args);
}
