"""Execute production registration parsing and readiness gates with a fake clock."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/passport_muse/sdk/noise_control.cpp').read_text()
start = source.index('            if (!s_register_acked && s_register_req_id[0])')
register = source[start:source.index('            handle_invoke_request', start)]
start = source.index('        const uint64_t now_us = esp_timer_get_time();', source.index('static session_result_t run_session'))
gate = source[start:source.index('        // Once registration is acked', start)]
prelude = r'''
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include "cJSON.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define IDENTITY_BODY_MAX 8192
static bool s_register_acked, s_register_rejected, s_heartbeat_registered, s_connected;
static char s_register_req_id[]="register-1";
static uint64_t now, registration_started_us, identity_started_us;
static unsigned available=40000, identity_requests, connections;
static bool SHOW_AGENT_NAME=true;
static std::string last_status;
static struct { bool requested,done; void *body; } identity;
static int tls,session,svc_scratch,env_scratch,ws_buf;
static uint64_t esp_timer_get_time(){return now;}
static unsigned heap_caps_get_free_size(int){return available;}
static unsigned largest=17000;
static unsigned heap_caps_get_largest_free_block(int){return largest;}
static void notify_status(const char *s){last_status=s;}
static void req_set_ready(bool ready){s_connected=ready; connections++;}
static bool identity_request(int,int,int,int,int){identity_requests++;return true;}
static void response(const char *text){
    const uint8_t *p=(const uint8_t*)text; size_t msg_len=strlen(text);
'''
checks = r'''
static void reset(){
    free(identity.body); identity={}; now=registration_started_us=identity_started_us=0;
    s_register_acked=s_register_rejected=s_heartbeat_registered=s_connected=false;
    identity_requests=connections=0; available=40000; largest=17000; SHOW_AGENT_NAME=true;
}
int main(){
    reset(); assert(!tick() && !s_connected && !identity_requests);
    response("{\"id\":\"other\",\"type\":\"res\",\"result\":{\"status\":\"registered\"}}");
    assert(!s_register_acked && !s_register_rejected);
    now=14999999; assert(!tick() && !s_connected);
    now=15000000; assert(tick() && last_status=="ws_registration_timeout");
    const char *bad[]={
      "{\"id\":\"register-1\",\"type\":\"res\",\"error\":{\"code\":403}}",
      "{\"id\":\"register-1\",\"type\":\"event\",\"result\":{\"status\":\"registered\"}}",
      "{\"id\":\"register-1\",\"type\":\"res\",\"result\":{\"status\":\"pending\"}}",
      "{\"id\":\"register-1\",\"type\":\"res\",\"result\":{\"status\":\"registered\"},\"error\":{}}"};
    for(auto message:bad){reset();response(message);assert(tick() && !s_connected && !s_heartbeat_registered);}
    const char *ok="{\"id\":\"register-1\",\"type\":\"res\",\"result\":{\"status\":\"registered\"}}";
    reset(); response(ok); assert(!tick() && identity_requests==1 && !s_connected);
    identity.body=malloc(8192); now=4999999; assert(!tick() && !s_connected);
    now=5000000; assert(!tick() && s_connected && !identity.body && identity.done);
    assert(connections==1); tick(); assert(connections==1);
    reset(); response(ok); available=24000; assert(!tick() && s_connected && !identity_requests);
    reset(); response(ok); largest=8192; assert(!tick() && s_connected && !identity_requests);
    reset(); response("{\"id\":\"register-1\",\"type\":\"res\",\"result\":{\"status\":\"registered\"},\"error\":null}");
    assert(s_register_acked && !s_register_rejected);
    reset(); SHOW_AGENT_NAME=false; response(ok); assert(!tick() && s_connected && !identity_requests);
    reset(); response(ok); tick(); identity.done=true; assert(!tick() && s_connected);
    reset(); response("broken JSON"); assert(!s_connected && !s_register_acked);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'readiness.cpp'
    path.write_text(prelude + register + '}\nstatic bool tick(){bool error=false;do{\n' + gate + '\n}while(false);return error;}\n' + checks)
    cjson = Path(os.environ.get('IDF_PATH', '/opt/esp-idf')) / 'components/json/cJSON'
    obj = Path(tmp) / 'cjson.o'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-I', str(cjson),
                    '-c', str(cjson / 'cJSON.c'), '-o', str(obj)], check=True)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(cjson), str(path), str(obj), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
print('Muse registration delay/rejection/timeout and optional identity gating: PASS')
