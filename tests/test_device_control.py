"""Exercise production device control, routing and settings storage with fault injection."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
settings=(root/'main/launcher/settings.cc').read_text()
def function(signature):
    start=settings.index(signature)
    opening=settings.index('{',start);depth=1;end=opening+1
    while depth:
        if settings[end]=='{':depth+=1
        if settings[end]=='}':depth-=1
        end+=1
    return settings[start:end]
prefix=r'''
#include "platform.h"
#include <atomic>
#include "launcher/settings.h"
#include "launcher/app_registry.h"
#include "launcher/battery_guard.h"
static const char *kNvsNamespace="system_prefs";
static uint8_t s_volume=80,s_brightness=80;
static uint16_t s_timeout=60;
static std::atomic<int64_t> s_outdoor_until_us{0};
static std::atomic<uint8_t> s_outdoor_restore{80};
static int master_brightness=80;
static int mock_soc=80;
static int bsp_battery_soc(){return mock_soc;}
uint8_t bsp_display_get_master_brightness(){return static_cast<uint8_t>(master_brightness);}
static constexpr int NVS_READONLY=1;
static constexpr int UI_THEME_CLASSIC=0,UI_THEME_COUNT=4;
using ui_theme_t=int;
static ui_theme_t s_theme=UI_THEME_CLASSIC;
static constexpr uint16_t kTimeouts[]={0,15,30,60,120,300};
static void ui_sfx_set_enabled(bool){}
static void ui_theme_set(int){}
static void bsp_display_set_master_brightness(uint8_t value){master_brightness=value;}
static void bsp_audio_set_master_volume(uint8_t value){master_volume=value;}
extern "C" void app_registry_set_screen_timeout(uint16_t){}
static int nvs_set_u8(int h,const char*k,uint8_t v){return nvs_set_blob(h,k,&v,sizeof(v));}
static int nvs_get_u8(int h,const char*k,uint8_t*v){size_t n=sizeof(*v);return nvs_get_blob(h,k,v,&n);}
static int nvs_get_u16(int h,const char*k,uint16_t*v){size_t n=sizeof(*v);return nvs_get_blob(h,k,v,&n);}
template <typename T,size_t N> T validated(const T(&values)[N],T value,T fallback){
    for(auto v:values)if(v==value)return value;return fallback;
}
'''
signatures=['void apply_settings()', 'void settings_bootstrap(void)',
            'bool save_percent(', 'uint8_t settings_get_volume(', 'uint8_t settings_get_brightness(',
            'bool settings_save_volume(', 'bool settings_save_brightness(']
code=prefix+'\n'.join(function(s) for s in signatures)+'\n'+(root/'tests/test_device_control.cc').read_text()
with tempfile.TemporaryDirectory(prefix='passport-control-') as temporary:
    source=Path(temporary)/'test.cc';target=Path(temporary)/'test'
    # The includes in the test are relative to tests/ in the real source tree.
    code=code.replace('#include "../main/', '#include "'+str(root/'main')+'/')
    source.write_text(code)
    timer_object=Path(temporary)/'pomodoro-core.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
                    '-Imain','-c','main/apps/pomodoro/pomodoro_core.c','-o',str(timer_object)],cwd=root,check=True)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                    '-g','-fsanitize=address,undefined','-Itests/reminder_mocks','-Itests/vendor/cjson',
                    '-Imain','-Icomponents/bsp/include',str(source),'tests/vendor/cjson/cJSON.c',
                    str(timer_object),'-o',str(target)],cwd=root,check=True)
    subprocess.run([str(target)],cwd=root,check=True)
