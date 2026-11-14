"""Run production Radio init/stop and catalog admission with worker failure fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
player = (ROOT / 'main/apps/radio/radio_player.cc').read_text()
player = player[player.index('bool radio_player_init()'):player.index('void radio_player_set_network')]
app = (ROOT / 'main/apps/radio/app_radio.cc').read_text()
catalog = app[app.index('bool catalog_cancelled()'):app.index('void start_catalog()')]
prelude = r'''
#include <cassert>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include "bsp_audio_workspace.h"
#include "voice_intent.h"
#include "apps/radio/radio_player.h"
#undef ESP_LOGI
#undef ESP_LOGW
#undef ESP_LOGE
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define ESP_AUDIO_ERR_OK 0
#define portMAX_DELAY 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
using TaskHandle_t=void*;
using StackType_t=uint32_t;
static std::atomic<TaskHandle_t> s_player_task;
static std::atomic<bool> s_shutdown_requested,s_network_connected,s_wanted_playing,s_stream_active;
static std::atomic<unsigned> s_generation,s_station_count,s_station_index;
static void *s_station_mutex,*s_player_stopped;
static bool s_decoders_registered;
static RadioStation *s_stations;
static constexpr unsigned kMaxStations=10,kStreamStackBytes=7168;
static int s_stream_tcb,token;
static const RadioStation kRadioPresets[1]={{"test","test","https://example.invalid",0}};
static unsigned step,fail_at,sync_live,decoder_live,audio_live;
static bool stopped=true;
static bool failing(){return ++step==fail_at;}
static void *xSemaphoreCreateMutex(){if(failing())return nullptr;sync_live++;return &token;}
static void *xSemaphoreCreateBinary(){return xSemaphoreCreateMutex();}
static void vSemaphoreDelete(void*p){assert(p && sync_live);sync_live--;}
static int xSemaphoreTake(void*,unsigned){return stopped;}
static void xSemaphoreGive(void*){}
static void player_task(void*){}
static void *xTaskCreateStatic(void(*)(void*),const char*,unsigned,void*,unsigned,StackType_t*,int*){return failing()?nullptr:&token;}
static void vTaskDelete(void*){}
static int bsp_audio_init_playback(){if(failing())return -1;audio_live=1;return 0;}
static void bsp_audio_deinit(){audio_live=0;}
static void bsp_audio_set_volume(int){}
static int esp_audio_dec_register_default(){if(failing())return -1;decoder_live++;return 0;}
static int esp_audio_simple_dec_register_default(){return esp_audio_dec_register_default();}
static void esp_audio_dec_unregister_default(){assert(decoder_live);decoder_live--;}
static void esp_audio_simple_dec_unregister_default(){esp_audio_dec_unregister_default();}
static unsigned load_setting(const char*,unsigned){return 0;}
std::size_t radio_player_station_count(){return s_station_count;}
RadioStation radio_player_station(std::size_t i){return s_stations[i];}
uint8_t radio_player_volume(){return 80;}
static void radio_ui_set_station(unsigned,unsigned,RadioStation){}
static void radio_ui_set_volume(unsigned){}
struct RadioLocation {char city[64],region[64],search_city[64],country_code[4];};
struct home_profile_t {char city[64],region[64],radio_city[64],country_code[4];};
static std::atomic<bool> s_catalog_cancel,s_radio_active,s_ui_alive,s_catalog_running,s_catalog_installed;
static void *s_catalog_commit=&token;
static bool idle=true,playing,fail_catalog_alloc,cancel_during_wait;
static unsigned queries,installs,allocations;
static void *catalog_calloc(size_t n,size_t s){allocations++;assert(idle);return fail_catalog_alloc?nullptr:std::calloc(n,s);}
void radio_player_set_playing(bool value){playing=value;}
bool radio_player_wait_idle(uint32_t){if(cancel_during_wait)s_catalog_cancel=true;return idle;}
static void home_profile_get(home_profile_t*p){*p={};}
static std::atomic<voice_intent::Request *> s_voice{nullptr};
static RadioLocation asked;
static std::string voice_filter;
static int chosen=-1;
static size_t radio_catalog_discover_location(RadioLocation preferred,RadioStation*out,size_t,RadioLocation*,bool(*)(),const char*filter){
 queries++;asked=preferred;voice_filter=filter?filter:"<browse>";out[0]={"北京新闻广播","","",0};out[1]={"北京交通广播","","",0};return 2;}
void radio_player_set_station(size_t index){chosen=static_cast<int>(index);}
static void radio_ui_set_location(const char*){}
bool radio_player_replace_stations_resuming(const RadioStation*,size_t){installs++;return true;}
'''
checks = r'''
int main(){
 using namespace audio_workspace;
 assert(acquire(Owner::Muse));assert(!radio_player_init());assert(release(Owner::Muse));
 step=fail_at=0;assert(radio_player_init());unsigned steps=step;
 assert(s_stations==(RadioStation*)radio::stations() && sizeof(RadioStation)*kMaxStations==4180);
 assert(!strcmp(s_stations[0].name,"test"));stopped=false;
 assert(!radio_player_deinit(20) && owner==Owner::Radio && s_stations && audio_live);
 assert(!acquire(Owner::Muse) && !radio_player_init());stopped=true;
 assert(radio_player_deinit(20) && owner==Owner::None && !s_stations && !sync_live && !decoder_live);
 assert(radio_player_deinit(0));
 for(unsigned f=1;f<=steps;f++){
  memset(storage,0x5a,kBytes);step=0;fail_at=f;assert(!radio_player_init());
  assert(owner==Owner::None && !s_stations && !sync_live && !decoder_live && !audio_live);
  for(unsigned i=0;i<radio::kCompressedBytes;i++)assert(aux()[i]==0x5a);
  for(auto p=radio::stations()+4180;p<storage+kBytes;p++)assert(*p==0x5a);
  step=fail_at=0;assert(radio_player_init());assert(radio_player_deinit(10));
 }
 s_radio_active=s_ui_alive=s_catalog_running=true;idle=false;
 catalog_worker(nullptr);assert(!queries && !allocations && playing && !s_catalog_running);
 s_catalog_running=true;idle=true;cancel_during_wait=true;
 catalog_worker(nullptr);assert(!queries && !allocations && !playing && !s_catalog_running);
 s_catalog_cancel=false;cancel_during_wait=false;s_catalog_running=true;fail_catalog_alloc=true;
 catalog_worker(nullptr);assert(!queries && playing && !s_catalog_running && !s_catalog_installed);
 fail_catalog_alloc=false;s_catalog_running=true;catalog_worker(nullptr);
 assert(queries==1 && installs==1 && playing && !s_catalog_running && s_catalog_installed && chosen==-1);
 // A voice request replaces the profile city once and picks the named station.
 auto *voice=new voice_intent::Request{};std::strcpy(voice->name,"北京");std::strcpy(voice->detail,"交通");
 s_voice=voice;s_catalog_running=true;catalog_worker(nullptr);
 assert(!strcmp(asked.city,"北京") && !strcmp(asked.country_code,"CN") && chosen==1 && !s_voice && voice_filter=="交通");
 voice=new voice_intent::Request{};std::strcpy(voice->name,"广东");s_voice=voice;s_catalog_running=true;
 catalog_worker(nullptr);assert(!strcmp(asked.city,"广东") && chosen==0 && !s_voice);
 chosen=-1;s_catalog_running=true;catalog_worker(nullptr);assert(!asked.city[0] && chosen==-1);
 puts("Radio shared stations: ownership, init failure, stop timeout, reuse; catalog timeout/cancel/OOM admission, once per session, voice city/station: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'radio.cpp'
    path.write_text(prelude + player + catalog.replace('std::calloc(', 'catalog_calloc(') + checks)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'tests/registry_mocks'),
                    '-I', str(ROOT / 'components/bsp/include'), '-I', str(ROOT / 'main'),
                    str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
