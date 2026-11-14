"""Run production music startup/search and both player admission paths offline."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'main/apps/netease/app_netease.cc').read_text()


def function(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[start:end]


code = r'''
#include "apps/netease/netease_model.h"
#include "apps/netease/netease_catalog.h"
#include "apps/netease/netease_voice_cache.h"
#include <cassert>
#include <cinttypes>
#include <string>
#include <utility>
#undef ESP_LOGI
#define ESP_LOGI(...) ((void)0)
using namespace netease;
enum class Cmd {Boot,BackHome};
static char cookie[kCookieBytes];static char *s_cookie=cookie;
static uint64_t s_uid;
static bool s_lists_pending;
static std::atomic<bool> s_browse_cancel{false};
static voice_intent::Request *s_intent;
static std::string saved_cookie="valid";
static bool cancelled,queued;
static unsigned accounts,lists,searches,plays,forgot;
static Result account_result=Result::Ok,search_result=Result::Ok;
static VoiceSearchCache cache{};
static int64_t now=1000000;
static int s_queue;
Model &netease::model(){static Model m{};return m;}
struct Guard {bool ok=true;};
static bool browse_cancelled(){return cancelled;}
static void show_message(const char*,bool,Cmd=Cmd::Boot){}
static const char *network_text(Result){return "error";}
static void load_device_id(){}
static bool wait_wifi(){return !cancelled;}
static std::string store_get_string(const char*,const char*,const char*){return saved_cookie;}
static bool store_set_string(const char*,const char*,const char*){return true;}
static bool store_erase(const char*,const char*){return true;}
static constexpr const char *kNs="netease";
static unsigned uxQueueMessagesWaiting(int){return queued;}
static uint32_t esp_random(){return 0;}
static void play_from(unsigned index){assert(index==0);++plays;}
static void forget_login(){++forgot;s_cookie[0]=0;s_uid=0;model().logged_in=false;}
Result netease::account(const char*,uint64_t*uid,char*nick,size_t cap,Cancelled){
 ++accounts;*uid=12;std::snprintf(nick,cap,"user");return account_result;
}
Result netease::user_playlists(const char*,uint64_t,Playlist*out,size_t,size_t*n,Playlist*fav,Cancelled){
 ++lists;*n=1;out[0]={};out[0].id=88;fav->id=99;return Result::Ok;
}
Result netease::search_songs(const char*,Song*out,uint64_t*ids,size_t,size_t*n,Cancelled){
 ++searches;*n=2;out[0]={};out[1]={};out[0].id=ids[0]=11;out[1].id=ids[1]=22;return search_result;
}
static bool take_voice_search(const char*q,Song*out,uint64_t*ids,size_t cap,size_t*n){
 return cache.take(q,out,ids,cap,n,now,false);
}
'''
code += '\n'.join(function(s) for s in (
    'void load_lists(', 'bool finish_login(', 'void boot()',
    'void shuffle_tracks(', 'void play_search('))
back_start = source.index('    case Cmd::BackHome: {')
back_end = source.index('    case Cmd::OpenList:', back_start)
code += '\nstatic void back_home(){switch(Cmd::BackHome){' + source[back_start:back_end] + 'default:break;}}\n'
code += r'''
static void reset(){
 model()={};model().open_list=-1;cancelled=queued=false;
 accounts=lists=searches=plays=forgot=0;cache={};s_intent=nullptr;s_lists_pending=false;
 account_result=search_result=Result::Ok;saved_cookie="valid";now=1000000;
}
int main(){
 reset();boot();assert(accounts==1&&lists==1&&model().logged_in);
 voice_intent::Request request{};
 for(auto type:{voice_intent::Source::Search,voice_intent::Source::Daily}){
  reset();request.source=type;s_intent=&request;boot();
  assert(accounts==1&&lists==0&&model().logged_in&&model().list_count==kHomeEntries);
  assert(model().page_id==Page::Lists);
  assert(s_lists_pending);back_home();assert(lists==1&&!s_lists_pending&&model().lists[0].id==99);
  back_home();assert(lists==1);
 }
 reset();request.source=voice_intent::Source::Favorites;s_intent=&request;boot();
 assert(accounts==1&&lists==1&&model().lists[0].id==99);
 reset();request.source=voice_intent::Source::Search;s_intent=&request;
 account_result=Result::Api;boot();assert(accounts==1&&lists==0&&forgot==1&&!model().logged_in);
 reset();s_intent=&request;account_result=Result::Network;boot();
 assert(accounts==1&&lists==0&&model().page_id!=Page::Lists&&!forgot);
 reset();saved_cookie="";s_intent=&request;boot();assert(!accounts&&!lists);
 Song offered[2]{};offered[0].id=11;offered[1].id=22;
 std::snprintf(offered[1].name,sizeof(offered[1].name),"chosen");
 reset();cache.save("query",offered,2,now);play_search("query",22,false);
 assert(!searches&&plays==1&&model().ids[0]==22&&model().page_count==2);
 assert(!std::strcmp(model().page[0].name,"chosen"));
 play_search("query",22,false);assert(searches==1&&plays==2); // consumed cache -> normal search
 reset();cache.save("query",offered,2,now);now+=46000000;play_search("query",22,false);
 assert(searches==1&&plays==1);
 reset();cache.save("query",offered,2,now);play_search("query",77,true);
 assert(!searches&&plays==1&&model().ids[0]==77&&model().page[0].id==77);
 reset();search_result=Result::Network;play_search("query",22,false);assert(searches==1&&!plays);
 reset();queued=true;cache.save("query",offered,2,now);play_search("query",22,false);assert(!plays);
 reset();cancelled=true;cache.save("query",offered,2,now);play_search("query",22,false);assert(!plays);
 puts("Music startup: account validation retained, library requests skipped only when unused; cached chosen-song queue, expiry/network/key/cancel fallback PASS");
}
'''

# Execute each production buffer gate with a paced HTTP reader. The initial
# frame is now admitted after two 4 KiB chunks, retaining the same refill gate.
gates = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#define pdMS_TO_TICKS(x) (x)
struct Buffer {std::size_t bytes;std::size_t available(){return bytes;}};
struct Reader {std::atomic<bool> finished;Buffer buffer;};
static Reader *active;static unsigned waits;
static void vTaskDelay(unsigned){assert(++waits<8);active->buffer.bytes+=4096;}
'''
for name, path, marker in (
    ('music', 'main/apps/netease/netease_player.cc', '        if (buffering) s_buffering'),
    ('radio', 'main/apps/radio/radio_player.cc', '            buffering = false;'),
):
    player = (root / path).read_text()
    start = player.index('const bool finished = reader.finished.load')
    end = player.index(marker, start)
    gates += f'bool {name}(size_t bytes,bool finished){{Reader reader{{finished,{{bytes}}}};active=&reader;waits=0;'
    gates += 'constexpr size_t capacity=16384;bool buffering=true;while(true){'
    gates += player[start:end] + 'return true;}return false;}\n'
gates += r'''
int main(){
 for(auto admit:{music,radio}){
  assert(admit(0,false)&&waits==2);
  assert(admit(4096,false)&&waits==1);
  assert(admit(8192,false)&&waits==0);
  assert(admit(1024,true)&&waits==0);
  assert(!admit(0,true)&&waits==0);
 }
 puts("Both production buffer gates: paced 8 KiB start/refill, short EOF, empty EOF PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='passport-media-startup-') as temporary:
    for name, text in (('music', code), ('gates', gates)):
        src = Path(temporary) / (name + '.cc')
        exe = src.with_suffix('')
        src.write_text(text)
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-fsanitize=address,undefined', '-g',
                        '-Itests/registry_mocks', '-Imain', '-Icomponents/bsp/include', str(src), '-o', str(exe)], cwd=root, check=True)
        subprocess.run([str(exe)], cwd=root, check=True)
