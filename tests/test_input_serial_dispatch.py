"""Run the production serial producer and mixed-input dispatcher with a bounded FIFO."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'main/main.cc').read_text()
def function(name):
    start=source.index('static void '+name+'(')
    opening=source.index('{',start)
    depth=1;end=opening+1
    while depth:
        if source[end]=='{':depth+=1
        if source[end]=='}':depth-=1
        end+=1
    return source[start:end]
code=r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>
enum app_btn_event_t { BTN_EVT_CLICK, BTN_EVT_PRESS, BTN_EVT_RELEASE };
struct KeyMessage { uint8_t button;app_btn_event_t event;uint16_t mv; };
static constexpr uint8_t kSerialCommand=0xff;
static constexpr uint8_t kPowerCommand=0xfe;
static std::vector<uint8_t> power_stages;
static void power_hold(uint8_t stage){power_stages.push_back(stage);}
static bool wifi_connected;
static unsigned scans;
struct WifiManager {
    static WifiManager &GetInstance(){static WifiManager w;return w;}
    bool IsConnected() const {return wifi_connected;}
    void ScanNow(){++scans;}
};
static void *s_key_queue=nullptr;
static std::deque<KeyMessage> queue;
static std::vector<int> handled;
static std::string input;
static size_t at;
static unsigned dropped;
static bool idle, eof_mode;
static int last_delay;
struct Done {};
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGW(...) (++dropped)
static int xQueueSend(void *,const KeyMessage *m,int){
    if(queue.size()==16)return 0;queue.push_back(*m);return 1;
}
static int fake_getc(FILE *){if(at==input.size()){if(eof_mode)return EOF;throw Done{};}return input[at++];}
static void vTaskDelay(int delay){last_delay=delay;throw Done{};}
static bool app_registry_background_idle(){return idle;}
static void handle_serial_command(int c){handled.push_back(c);}
static void app_registry_dispatch_key(uint8_t b,app_btn_event_t e,uint16_t mv){
    assert(b==0 && mv==15);handled.push_back(e==BTN_EVT_PRESS?-1:-2);
}
#define fgetc fake_getc
'''+function('key_post_wait')+'\n'+function('dispatch_input_message')+'\n'+function('serial_cmd_task')+r'''
int main(){
    queue.push_back({0,BTN_EVT_PRESS,15});
    input="m[]v?13";
    try{serial_cmd_task(nullptr);}catch(Done&){}
    assert(handled.empty()); // reader never executes app/UI work on its small stack
    queue.push_back({0,BTN_EVT_RELEASE,15});
    while(!queue.empty()){auto msg=queue.front();queue.pop_front();dispatch_input_message(msg);}
    assert((handled==std::vector<int>{-1,'m','[',']','v','?','1','3',-2}));
    // Offline: only the press (not the release) asks Wi-Fi to rescan.
    assert(scans==1);
    wifi_connected=true;handled.clear();
    dispatch_input_message({0,BTN_EVT_PRESS,15});dispatch_input_message({kPowerCommand,BTN_EVT_CLICK,2});
    assert(scans==1 && (handled==std::vector<int>{-1}) && (power_stages==std::vector<uint8_t>{2}));
    input=std::string(20,'v');at=0;handled.clear();
    try{serial_cmd_task(nullptr);}catch(Done&){}
    assert(queue.size()==16 && dropped==4 && handled.empty());
    // A drained nonblocking console must yield in both states. Wake restores
    // normal polling, and a burst is drained before entering the idle delay.
    eof_mode=true;
    for(bool asleep : {false,true,false}) {
        idle=asleep;queue.clear();input="v?";at=0;last_delay=0;
        try{serial_cmd_task(nullptr);}catch(Done&){}
        assert(queue.size()==2 && queue[0].mv=='v' && queue[1].mv=='?');
        assert(last_delay==(asleep?250:50));
    }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)/'input.cpp';p.write_text(code)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',str(p),'-o',str(p.with_suffix(''))],check=True)
    subprocess.run([str(p.with_suffix(''))],check=True)
print('Unified input: serial producer isolation, mixed FIFO order, bounded overflow, power routing, offline key rescan: PASS')
