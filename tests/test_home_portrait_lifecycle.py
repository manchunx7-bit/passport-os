"""Exercise production portrait allocation, rendering fallback, refresh and release."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/apps/home/app_home.cc').read_text()
read = source[source.index('esp_err_t read_portrait'):source.index('// 显示用 A8')]
apply = source[source.index('void apply_avatar'):source.index('// ---- 天气像素图标:')]
cleanup = source[source.index('    if (s_portrait_pixels != nullptr)', source.index('void home_stop()')):
                 source.index('    ESP_LOGI(kTag, "Home app stopped', source.index('void home_stop()'))]
prelude = r'''
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <set>
#define HOME_AVATAR_BYTES 4976
#define HOME_AVATAR_MAX_BYTES 6664
#define HOME_PORTRAIT_PIXELS 14400
#define HOME_PORTRAIT_I4_BYTES 7264
#define HOME_PORTRAIT_W 120
#define LV_COLOR_FORMAT_A8 1
#define LV_COLOR_FORMAT_I4 2
#define LV_OBJ_FLAG_HIDDEN 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND -2
#define ESP_ERR_NO_MEM -3
#define ESP_LOGW(...) ((void)0)
using esp_err_t=int;
static uint8_t *s_portrait_packed,*s_portrait_pixels;
static struct {struct {int cf,stride;}header;size_t data_size;const void *data;}s_portrait_dsc;
static int image,placeholder;
static void *s_avatar_img=&image,*s_portrait_ph=&placeholder;
static const void *displayed;
static bool hidden=true,fail_read;
static int allocations,fail_at,version=1;
static std::set<void*>live;
static void *allocate(size_t n){if(++allocations==fail_at)return nullptr;void*p=malloc(n);assert(p);live.insert(p);return p;}
static void release(void*p){if(p){assert(p!=displayed);assert(live.erase(p)==1);free(p);}}
static inline bool home_portrait_is_color(const uint8_t*d){return d[0]=='P'&&d[1]=='T'&&d[2]=='3'&&d[3]=='6';}
static int home_profile_read_avatar(uint8_t*p,size_t n){assert(n==HOME_AVATAR_MAX_BYTES);if(fail_read)return ESP_FAIL;memset(p,0,n);p[4]=version;return ESP_OK;}
static void lv_image_set_src(void*,const void*p){displayed=p;}
static void lv_image_cache_drop(const void*){}
static void lv_obj_set_style_image_recolor(void*,int,int){}
static int lv_color_hex(int v){return v;}
static int ui_theme_portrait_color(int v){return v;}
static void lv_obj_clear_flag(void*p,int){if(p==s_avatar_img)hidden=false;}
static void lv_obj_add_flag(void*p,int){if(p==s_avatar_img)hidden=true;}
static void expand_portrait(){assert(s_portrait_packed && s_portrait_pixels);memset(s_portrait_pixels,0x5a,s_portrait_dsc.data_size);}
#define malloc allocate
#define free release
'''
checks = r'''
int main(){
 assert(read_portrait(false)==ESP_FAIL && !allocations);apply_avatar(false);assert(hidden);
 fail_at=1;assert(read_portrait(true)==ESP_ERR_NO_MEM);apply_avatar(false);assert(hidden&&!displayed);
 fail_at=0;assert(read_portrait(true)==ESP_OK);fail_at=allocations+1;
 apply_avatar(true);assert(hidden&&!displayed&&live.size()==1);
 fail_at=0;apply_avatar(true);assert(!hidden&&s_portrait_dsc.data==s_portrait_pixels&&live.size()==2);
 int before=allocations;assert(read_portrait(true)==ESP_OK);apply_avatar(true);assert(allocations==before);
 version=0;assert(read_portrait(true)==ESP_OK);apply_avatar(true);assert(!hidden&&s_portrait_dsc.data_size==14400&&live.size()==2);
 fail_read=true;apply_avatar(read_portrait(true)==ESP_OK);assert(hidden&&!displayed);
 cleanup();assert(!s_portrait_pixels&&!s_portrait_packed&&!s_portrait_dsc.data&&live.empty());
 cleanup();fail_read=false;version=1;assert(read_portrait(true)==ESP_OK);apply_avatar(true);
 lv_image_set_src(s_avatar_img,nullptr);cleanup();assert(live.empty());
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'home.cpp'
    path.write_text(prelude + read + apply + '\nstatic void cleanup(){\n' + cleanup + '}\n' + checks)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
print('Home portrait: absent/OOM/read failure, refresh/reformat, release and re-entry: PASS')
