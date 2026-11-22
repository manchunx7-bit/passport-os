"""Exercise real bounded allocator wrappers with the same link interception as IDF."""
from pathlib import Path
import platform
import subprocess,tempfile
if platform.system()=='Darwin':
    print('Muse TLS fixed storage: SKIP (GNU ld --wrap interception is Linux-only)')
    raise SystemExit(0)
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp)
    headers={
      'esp_err.h':'#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM -1\n#define ESP_ERR_INVALID_STATE -2\n',
      'mbedtls/ssl.h':'#pragma once\nstruct mbedtls_ssl_context { void *buf; };\n',
      'mbedtls/esp_mbedtls_dynamic.h':'#include "esp_err.h"\n#include "mbedtls/ssl.h"\nesp_err_t esp_mbedtls_dynamic_set_rx_buf_static(mbedtls_ssl_context *);\n',
      'mbedtls/platform_util.h':'#pragma once\n#include <cstddef>\nvoid mbedtls_platform_zeroize(void *, size_t);\n',
      'freertos/FreeRTOS.h':'#pragma once\nusing TaskHandle_t=void *;\n',
      'freertos/task.h':'#include "freertos/FreeRTOS.h"\nTaskHandle_t xTaskGetCurrentTaskHandle();\n'}
    for name,body in headers.items():
        p=d/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
    (d/'heap.cpp').write_text(r'''
#include <cstdlib>
#include <cstddef>
unsigned heap_allocs,heap_frees;
extern "C" void *esp_mbedtls_mem_calloc(size_t n,size_t s) {++heap_allocs;return calloc(n,s);}
extern "C" void esp_mbedtls_mem_free(void *p) {if(p)++heap_frees;free(p);}
''')
    (d/'sdk.cpp').write_text(r'''
#include "muse_tls_memory.h"
#include <cstring>
#include <cassert>
#include <cstddef>
#include <limits>
extern "C" void *esp_mbedtls_mem_calloc(size_t,size_t);
extern "C" void esp_mbedtls_mem_free(void *);
static auto alloc_fn=esp_mbedtls_mem_calloc;
static auto free_fn=esp_mbedtls_mem_free;
void *task=(void*)1;
void *xTaskGetCurrentTaskHandle(){return task;}
void mbedtls_platform_zeroize(void *p,size_t n){memset(p,0,n);}
size_t requested=8866;
bool extra_allocation=false,other_task=false;
unsigned erased;
void *last_buffer;
void sdk_free(mbedtls_ssl_context *s) {
    free_fn(s->buf);s->buf=nullptr;
    if(last_buffer){auto *p=(unsigned char*)last_buffer;for(size_t i=0;i<9216;++i)assert(p[i]==0);++erased;last_buffer=nullptr;}
}
esp_err_t esp_mbedtls_dynamic_set_rx_buf_static(mbedtls_ssl_context *s) {
    if(other_task) {task=(void*)2;void *p=alloc_fn(1,8866);assert(p);free_fn(p);task=(void*)1;}
    s->buf=alloc_fn(1,requested);
    if(!s->buf)return ESP_ERR_NO_MEM;
    if(extra_allocation)assert(!alloc_fn(1,1));
    last_buffer=s->buf;
    for(size_t i=0;i<requested;++i)assert(((unsigned char*)s->buf)[i]==0);
    memset(s->buf,0xa5,requested);
    return ESP_OK;
}
''')
    (d/'test.cpp').write_text(r'''
#include "muse_tls_memory.h"
#include <cassert>
#include <cstddef>
#include <limits>
extern unsigned heap_allocs,heap_frees,erased;
extern size_t requested;
extern bool extra_allocation,other_task;
void sdk_free(mbedtls_ssl_context *);
extern "C" void *esp_mbedtls_mem_calloc(size_t,size_t);
extern "C" void esp_mbedtls_mem_free(void *);
int main(){
    mbedtls_ssl_context a{},b{};
    assert(muse_tls_reserve_rx(nullptr)==ESP_ERR_INVALID_STATE);
    void *p=esp_mbedtls_mem_calloc(1,100);assert(p);esp_mbedtls_mem_free(p);
    extra_allocation=true;other_task=true;
    for(int i=0;i<1000;++i){
        unsigned before=heap_allocs;
        assert(muse_tls_reserve_rx(&a)==ESP_OK);
        assert(heap_allocs==before+1); // only the unrelated TLS task uses heap
        assert(muse_tls_reserve_rx(&b)==ESP_ERR_INVALID_STATE);
        sdk_free(&a);
    }
    assert(erased==1000 && heap_allocs==heap_frees);
    other_task=false;
    for(size_t size:{size_t(0),size_t(9217),std::numeric_limits<size_t>::max()}){
        requested=size;assert(muse_tls_reserve_rx(&a)==ESP_ERR_NO_MEM);
    }
    requested=8866;assert(muse_tls_reserve_rx(&a)==ESP_OK);sdk_free(&a);
    assert(heap_allocs==heap_frees);
}
'''.replace('#include <limits>','#include <limits>\n#include <initializer_list>'))
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',
      '-I'+str(d),'-I'+str(root/'components/passport_muse/sdk'),
      str(root/'components/passport_muse/sdk/muse_tls_memory.cpp'),str(d/'heap.cpp'),str(d/'sdk.cpp'),str(d/'test.cpp'),
      '-Wl,--wrap=esp_mbedtls_mem_calloc','-Wl,--wrap=esp_mbedtls_mem_free','-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
print('Muse TLS fixed storage: 1000 sessions, exclusive ownership, erasure, bounds, unrelated TLS passthrough: PASS')
