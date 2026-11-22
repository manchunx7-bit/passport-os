# SPDX-License-Identifier: Apache-2.0
"""Run production BLE provisioning dispatch with delayed tasks and failing allocators."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PairingDispatch(unittest.TestCase):
    def test_provision_ownership_memory_failure_and_session_guards(self):
        source = (ROOT / 'components/passport_muse/sdk/ble_server.c').read_text()
        job_type = source[source.index('#define DISPATCH_QUEUE_LEN'):
                          source.index('static StaticQueue_t s_dispatch_queue_buf;')]
        workers = source[source.index('static BaseType_t queue_dispatch('):
                         source.index('static void dispatch_worker(')]
        provision = source[source.index('typedef struct {', source.index('} dispatch_job_t;')):
                           source.index('static void scan_task(')]
        optional = source[source.index('static const char *optional_ota_url('):
                          source.index('static void unpair_task(')]
        dispatch = source[source.index('static bool is_sensitive_setup_action('):
                          source.index('static void dispatch_command(const ')]
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "cJSON.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_8BIT 0
#define pdPASS 1
#define pdFAIL 0
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
static atomic_uint s_dispatch_tasks;
static atomic_bool s_shutting_down, s_device_info_pending;
static bool s_plaintext_status_blocked;
static bool confirmed=true, session_valid=true, fail_task;
static uint32_t generation=7;
static size_t live_json, expected_json, live_args;
static int fail_at, allocations, callbacks, disconnected, tasks;
static void *pending_job;
static char last_status[80], command[4096];
static void *json_malloc(size_t n) { void *p=malloc(n); if(p) live_json++; return p; }
static void json_free(void *p) { if(p) { assert(live_json); live_json--; free(p); } }
static void *test_malloc(size_t n) {
    if(++allocations==fail_at) return NULL;
    void *p=malloc(n); if(p) live_args++; return p;
}
static void *test_calloc(size_t n,size_t size) {
    void *p=test_malloc(n*size); if(p) memset(p,0,n*size); return p;
}
static void test_free(void *p) { if(p) { assert(live_args); live_args--; free(p); } }
typedef void *QueueHandle_t;
static int queue_token;
static QueueHandle_t s_dispatch_queue=&queue_token;
static BaseType_t queue_send(QueueHandle_t q,const void *item,unsigned ticks);
#define xQueueSend queue_send
static void stack_monitor_record(void *p) { (void)p; }
static inline unsigned heap_caps_get_free_size(int c) { (void)c; return 20000; }
static inline unsigned heap_caps_get_largest_free_block(int c) { (void)c; return 10000; }
static void mbedtls_platform_zeroize(void *p,size_t n) { memset(p,0,n); }
static bool link_pairing_provisioning_session_valid(uint32_t g) { return session_valid && g==7; }
static uint32_t link_pairing_mark_provisioning_active(void) { return generation; }
static bool link_pairing_session_confirmed(void) { return confirmed; }
static bool config_setup_complete(void) { return false; }
static bool link_pairing_plaintext_setup_blocked(void) { return true; }
static void ble_server_send_status(const char *s) { snprintf(last_status,sizeof(last_status),"%s",s); }
static void ble_server_send_pairing_status(const char *s,uint32_t g) { (void)g; ble_server_send_status(s); }
static void ble_server_disconnect_pairing_session(uint32_t g) { (void)g; disconnected++; }
static void ble_server_delayed_disconnect(int ms) { (void)ms; }
static void ble_server_send_chunked(const char *s) { (void)s; }
static const char *link_pairing_handle_client_hello(cJSON *r,char **out) { (void)r; (void)out; return "unused"; }
static const char *link_pairing_decrypt_command(cJSON *r,char **out) {
    (void)r; expected_json=0;
    *out=test_malloc(strlen(command)+1); assert(*out); strcpy(*out,command); return NULL;
}
static uint32_t link_pairing_handle_client_finished(void) { return 0; }
static void link_pairing_reset(void) {}
typedef struct { const char *version; } esp_app_desc_t;
static const esp_app_desc_t *esp_app_get_description(void) {
    static const esp_app_desc_t desc={"test"}; return &desc;
}
typedef struct { char *url; bool force; } ota_args_t;
static void scan_task(void *p) { (void)p; }
static void ble_ota_task(void *p) { (void)p; }
static void unpair_task(void *p) { (void)p; }
static void device_info_task(void *p) { (void)p; }
static void pairing_client_finished_task(void *p) { (void)p; }
static void on_provision(const char *ssid,const char *password,const char *access,
                         const char *refresh,const char *username,const char *ota,
                         bool force,const char *api,const char *api2,const char *host,uint32_t gen) {
    callbacks++;
    assert(!strcmp(ssid,"test-network") && !strcmp(password,"test-password"));
    assert(strlen(access)==1000 && strlen(refresh)==1000);
    assert(access[0]=='a' && refresh[0]=='r' && gen==7);
    assert(!strcmp(username,"test-user") && !strcmp(ota,"https://example.test/ota"));
    assert(force && !strcmp(api,"https://example.test/v1"));
    assert(!strcmp(api2,"https://example.test/v2") && !strcmp(host,"example.test"));
}
static struct { void (*on_provision)(const char *,const char *,const char *,const char *,
                    const char *,const char *,bool,const char *,const char *,const char *,uint32_t); } s_cb={on_provision};
#define malloc test_malloc
#define calloc test_calloc
#define free test_free
'''
        queue_send = r'''
    (void)ticks; assert(q==s_dispatch_queue);
    const dispatch_job_t *job=item;
    /* Both inner command and encrypted envelope must be gone. */
    assert(live_json==expected_json);
    tasks++;
    if(fail_task) return pdFAIL;
    assert(!pending_job && job->fn==provision_task);
    static dispatch_job_t held; held=*job; pending_job=&held; return pdPASS;
'''
        cases = r'''
#undef malloc
#undef calloc
#undef free
static void run(bool encrypted) {
    const char *outer="{\"action\":\"pairing_encrypted\"}";
    expected_json=0;
    dispatch_command_ex((const uint8_t *)(encrypted?outer:command),strlen(encrypted?outer:command),!encrypted);
    assert(!live_json);
    if(pending_job) {
        const dispatch_job_t *job=pending_job; pending_job=NULL; run_dispatch_job(job);
    }
    assert(!live_args && !s_dispatch_tasks);
}
int main(void) {
    cJSON_Hooks hooks={json_malloc,json_free}; cJSON_InitHooks(&hooks);
    char access[1001],refresh[1001]; memset(access,'a',1000); access[1000]=0;
    memset(refresh,'r',1000); refresh[1000]=0;
    snprintf(command,sizeof(command),"{\"action\":\"provision_v2\",\"ssid\":\"test-network\","
        "\"password\":\"test-password\",\"access_token\":\"%s\",\"refresh_token\":\"%s\","
        "\"token_type\":\"device\",\"username\":\"test-user\",\"ota_url\":\"https://example.test/ota\","
        "\"ota_force\":true,\"api_url\":\"https://example.test/v1\","
        "\"api_url_v2\":\"https://example.test/v2\",\"noise_host\":\"example.test\"}",access,refresh);
    run(false); assert(callbacks==1 && tasks==1);
    run(true); assert(callbacks==2 && tasks==2);
    fail_task=true; run(true);
    assert(callbacks==2 && disconnected==1 && !strcmp(last_status,"error_operation_in_progress"));
    fail_task=false;
    /* Args structure and each of its nine string copies fail independently. */
    for(int i=1;i<=10;i++) {
        allocations=0; fail_at=i; run(false);
        assert(callbacks==2 && !strcmp(last_status,"error_operation_in_progress"));
    }
    fail_at=0; run(true); assert(callbacks==3);
    session_valid=false; run(false); assert(callbacks==3); session_valid=true;
    generation=0; run(false); assert(callbacks==3 && disconnected==2); generation=7;
    confirmed=false; run(true);
    assert(callbacks==3 && !strcmp(last_status,"error_pairing_confirm_required"));
    confirmed=true; s_shutting_down=true; run(false); assert(callbacks==3);
    s_shutting_down=false; run(true); assert(callbacks==4);
    puts("BLE provision dispatch: ownership, early JSON release, OOM, session guards: PASS");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            c = Path(directory) / 'dispatch.c'
            send = ('static BaseType_t queue_send(QueueHandle_t q,const void *item,unsigned ticks) {'
                    + queue_send + '}\n')
            c.write_text(prelude + job_type + workers + provision + send + optional + dispatch + cases,
                         encoding='utf-8')
            binary = c.with_suffix('')
            cjson = Path(os.environ.get('IDF_PATH', '/opt/esp-idf')) / 'components/json/cJSON'
            compiled = subprocess.run([
                os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                '-Wno-unused-parameter', '-Wno-unused-function', '-fsanitize=address,undefined',
                '-I', str(cjson), str(c), str(cjson / 'cJSON.c'), '-o', str(binary)
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
