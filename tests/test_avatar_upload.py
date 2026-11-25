"""Exercise the actual avatar HTTP handler with fragmented reads and failures."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
baseline = len(sys.argv) > 1
source_root = Path(sys.argv[1]).resolve() if baseline else ROOT
source = (source_root / 'main/apps/home/home_upload.cc').read_text(encoding='utf-8')
start = source.index('static esp_err_t handler_avatar_post(')
handler = source[start:source.index('static esp_err_t handler_share_post(', start)]
get_handler = ''
if 'static esp_err_t handler_avatar_get(' in source and 'static esp_err_t send_avatar_upload_error(' in source:
    get_start = source.index('static esp_err_t handler_avatar_get(')
    get_handler = source[get_start:source.index('static esp_err_t send_avatar_upload_error(', get_start)]
helpers = ''
if 'static esp_err_t send_avatar_upload_error(' in source:
    first = source.index('static esp_err_t send_avatar_upload_error(')
    helpers = source[first:start]
fixture = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "apps/home/home_portrait.h"
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101;
constexpr int ESP_ERR_INVALID_SIZE=0x104, ESP_ERR_TIMEOUT=0x107;
constexpr int HTTPD_SOCK_ERR_TIMEOUT=-3;
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
struct httpd_req_t {int content_len;};
static bool authorized=true, fail_store=false, mock_color_avatar=false, mock_rgb565_avatar=false;
static int status, aborts, reads;
static int64_t now;
static size_t consumed, written, get_sent_len;
static bool committed;
static std::string response;
static std::vector<int> events;
static int64_t esp_timer_get_time(){return now;}
static const char *esp_err_to_name(int){return "ESP_ERR_NO_MEM";}
static bool token_ok(httpd_req_t*){return authorized;}
static int send_json(httpd_req_t*,int code,const char *text){status=code;response=text;return ESP_OK;}
static void httpd_resp_set_type(httpd_req_t*,const char*){}
static void httpd_resp_set_hdr(httpd_req_t*,const char*,const char*){}
static esp_err_t httpd_resp_send(httpd_req_t*,const char*,size_t len){get_sent_len=len;return ESP_OK;}
static bool home_profile_avatar_is_rgb565(){return mock_rgb565_avatar;}
static esp_err_t home_profile_read_avatar(uint8_t* dst,size_t cap){
 if(mock_rgb565_avatar){assert(cap>=HOME_AVATAR_RGB565_BYTES);home_portrait_init_rgb565_header(dst,100);return ESP_OK;}
 assert(cap>=HOME_AVATAR_MAX_BYTES);
 if(mock_color_avatar) home_portrait_init_color_header(dst,0,50);
 else home_portrait_init_header(dst,3);
 return ESP_OK;
}
static int httpd_req_recv(httpd_req_t*,char *data,size_t cap){
 ++reads;
 int n=events.empty()?static_cast<int>(cap):events.front();
 if(!events.empty())events.erase(events.begin());
 now+=n==HTTPD_SOCK_ERR_TIMEOUT?5000000:1000;
 if(n<=0)return n;
 n=std::min(n,static_cast<int>(cap));memset(data,0x5a,n);consumed+=n;return n;
}
static void home_profile_abort_avatar_upload(){++aborts;written=0;}
static int home_profile_write_avatar_chunk(const uint8_t*,size_t n,bool first,bool last,size_t total){
 if(fail_store)return ESP_ERR_NO_MEM;
 if(first)written=0;written+=n;
 if(last){assert(written==total);committed=true;}
 return ESP_OK;
}
'''
cases = r'''
static void reset(std::vector<int> input={}){
 events=input;status=aborts=reads=0;now=0;consumed=written=0;
 response.clear();committed=false;fail_store=false;authorized=true;
}
int main(){
 for(int size:{HOME_AVATAR_RGB565_BYTES,HOME_AVATAR_MAX_BYTES,HOME_AVATAR_BYTES,HOME_AVATAR_V2_BYTES,HOME_AVATAR_LEGACY_BYTES}){
  reset({1,7,128,511});httpd_req_t req{size};handler_avatar_post(&req);
  assert(status==200&&committed&&consumed==static_cast<size_t>(size));
 }
 httpd_req_t req{HOME_AVATAR_BYTES};
 reset({128,HTTPD_SOCK_ERR_TIMEOUT});handler_avatar_post(&req);
 if(BASELINE){assert(status==400&&!committed&&aborts==1);std::cout<<"rc.48 REPRODUCED: one receive timeout rejects an otherwise valid avatar\n";}
 else{assert(status==200&&committed);std::cout<<"Avatar receive: fragmented body and one temporary timeout recover: PASS\n";}
 reset({128,HTTPD_SOCK_ERR_TIMEOUT,HTTPD_SOCK_ERR_TIMEOUT});handler_avatar_post(&req);
 assert(status==400&&!committed&&aborts==1);if(!BASELINE)assert(now==10001000);
 reset({128,0});handler_avatar_post(&req);assert(status==400&&!committed&&aborts==1);
 reset();fail_store=true;handler_avatar_post(&req);assert(status==400&&!committed);
 if(!BASELINE){assert(aborts==1);assert(response.find("avatar_store")!=std::string::npos);assert(response.find("ESP_ERR_NO_MEM")!=std::string::npos);}
 reset();req.content_len=5000;handler_avatar_post(&req);assert(status==400&&reads==0);
 reset();authorized=false;req.content_len=HOME_AVATAR_BYTES;handler_avatar_post(&req);assert(status==401&&reads==0);
#if TEST_AVATAR_GET
 reset();mock_rgb565_avatar=false;mock_color_avatar=false;get_sent_len=0;handler_avatar_get(&req);assert(get_sent_len==HOME_AVATAR_BYTES);
 reset();mock_rgb565_avatar=false;mock_color_avatar=true;get_sent_len=0;handler_avatar_get(&req);assert(get_sent_len==HOME_AVATAR_COLOR_BYTES);
 reset();mock_rgb565_avatar=true;get_sent_len=0;handler_avatar_get(&req);assert(get_sent_len==HOME_AVATAR_RGB565_BYTES);
 mock_rgb565_avatar=false;
#endif
 std::cout<<"Avatar HTTP: legacy/current sizes, bad size/auth, disconnect, deadline and storage failure: PASS\n";
}
'''.replace('BASELINE', 'true' if baseline else 'false').replace('TEST_AVATAR_GET', '1' if get_handler else '0')
with tempfile.TemporaryDirectory(prefix='passport-avatar-http-') as directory:
    work = Path(directory)
    cpp = work / 'test.cc'
    cpp.write_text(fixture + get_handler + helpers + handler + cases, encoding='utf-8')
    binary = work / 'test'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-misleading-indentation',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(source_root / 'main'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
