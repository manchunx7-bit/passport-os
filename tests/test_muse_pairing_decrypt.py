"""Check production decrypt buffer ownership, bounds and failure cleanup."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/passport_muse/sdk/link_pairing.c').read_text()
decrypt = source[source.index('const char *link_pairing_decrypt_command('):
                 source.index('uint32_t link_pairing_handle_client_finished(')]
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#define ESP_LOGE(...) ((void)0)
#define GCM_TAG_BYTES 16
static const char *s_session_id_b64="session";
static uint8_t s_rx_key[32], scratch[3075];
static uint64_t s_rx_counter;
static bool locked, expired, available=true, fail_alloc, fail_base64, fail_auth;
static unsigned resets, allocations, frees, decrypts;
static void lock_take(void) { assert(!locked); locked=true; }
static void lock_give(void) { assert(locked); locked=false; }
static bool expired_locked(void) { return expired; }
static bool session_keys_available_locked(void) { return true; }
static void reset_locked(void) { resets++; }
static bool json_string(cJSON *r,const char *key,const char **out) {
    cJSON *v=cJSON_GetObjectItem(r,key);
    if(!cJSON_IsString(v)||!v->valuestring||!*v->valuestring)return false;
    *out=v->valuestring;return true;
}
static bool parse_u64_decimal(const char *s,uint64_t *v) { *v=strtoull(s,NULL,10);return true; }
static uint8_t *muse_pairing_cipher_buffer(size_t n) {
    assert(locked); assert(n<=sizeof(scratch)); return available?scratch:NULL;
}
static void *test_malloc(size_t n) { assert(n<=3076); allocations++;return fail_alloc?NULL:malloc(n); }
static void test_free(void *p) { if(p){frees++;free(p);} }
static void mbedtls_platform_zeroize(void *p,size_t n) { memset(p,0,n); }
static bool base64url_decode(const char *s,uint8_t *out,size_t cap,size_t *len) {
    if(fail_base64)return false;
    *len=!strcmp(s,"tag")?16:strlen(s)/4*3;
    assert(*len<=cap);memset(out,0xa5,*len);return true;
}
static bool aes_gcm_decrypt(const uint8_t *key,const uint8_t *cipher,size_t n,
                           const uint8_t *tag,uint8_t direction,uint64_t counter,uint8_t *plain) {
    (void)key;(void)tag;(void)direction;(void)counter;
    // In place: the AES DMA must not see a heap buffer that may be RTC memory.
    assert(locked && cipher==scratch && plain==scratch && cipher[0]==0xa5);
    decrypts++;memset(plain,'x',n);return !fail_auth;
}
#define malloc test_malloc
#define free test_free
'''
cases = r'''
#undef malloc
#undef free
static void run(cJSON *root,bool success) {
    char *plain=(char*)1;unsigned before=resets;
    const char *error=link_pairing_decrypt_command(root,&plain);
    assert(!locked);
    if(success) {assert(!error && plain && plain[0]=='x');test_free(plain);}
    else {assert(error && !plain && resets==before+1);}
    for(unsigned i=0;i<sizeof(scratch);++i)assert(!scratch[i]);
}
int main(void) {
    cJSON *r=cJSON_CreateObject();cJSON_AddStringToObject(r,"session_id","session");
    cJSON_AddStringToObject(r,"counter","0");cJSON_AddStringToObject(r,"tag","tag");
    char cipher[4101];memset(cipher,'a',4100);cipher[4096]=0;
    cJSON_AddStringToObject(r,"ciphertext",cipher);
    for(unsigned i=0;i<1000;++i){s_rx_counter=0;run(r,true);assert(s_rx_counter==1);}
    assert(allocations==1000 && frees==1000 && decrypts==1000);
    s_rx_counter=0;fail_alloc=true;run(r,false);fail_alloc=false;
    fail_base64=true;run(r,false);fail_base64=false;
    fail_auth=true;run(r,false);fail_auth=false;
    available=false;run(r,false);available=true;
    expired=true;run(r,false);expired=false;
    unsigned before=allocations;
    cipher[4096]='a';cipher[4100]=0;
    cJSON_ReplaceItemInObject(r,"ciphertext",cJSON_CreateString(cipher));run(r,false);
    cJSON_ReplaceItemInObject(r,"ciphertext",cJSON_CreateString("a"));run(r,false);
    assert(allocations==before);cJSON_Delete(r);
    assert(frees+1==allocations); // Only the injected malloc failure has no free.
}
'''
with tempfile.TemporaryDirectory() as tmp:
    file = Path(tmp) / 'decrypt.c'
    file.write_text(prelude + decrypt + cases)
    cjson = Path(os.environ.get('IDF_PATH', '/opt/esp-idf')) / 'components/json/cJSON'
    binary = file.with_suffix('')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
                    '-fsanitize=address,undefined', '-I' + str(cjson), str(file),
                    str(cjson / 'cJSON.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Pairing decrypt: bounded scratch, 1000 lifetimes, counters, OOM/auth cleanup: PASS')
