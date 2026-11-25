"""Reproduce slow TCP connect: select clears fd sets on each timed-out poll."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('tls_patch', root / 'tools/patch_esp_tls.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
# ESP-IDF 5.5.3 esp_tls.c CONNECTING preamble (Espressif, Apache-2.0).
original = '''case ESP_TLS_CONNECTING:
        if (cfg && cfg->non_block) {
            ESP_LOGD(TAG, "connecting...");
            struct timeval tv;
            ms_to_timeval(cfg->timeout_ms, &tv);

            /* In case of non-blocking I/O, we use the select() API to check whether
               connection has been established or not*/
            if (select('''
sdk = Path('/opt/esp-idf/components/esp-tls/esp_tls.c')
# Exercise the actual SDK used by the build, while keeping the failing baseline.
patched = module.patch(sdk.read_text() if sdk.exists() else original)
assert module.patch(patched) == patched

def preamble(source):
    start = source.index('case ESP_TLS_CONNECTING:')
    start = source.index('if (cfg && cfg->non_block) {', start)
    start = source.index('{', start) + 1
    end = source.index('if (select(', start)
    return source[start:end]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <sys/select.h>
#include <stdio.h>
#define ESP_LOGD(...)
typedef struct { int sockfd; fd_set rset, wset; } TLS;
typedef struct { int timeout_ms; } Config;
static void ms_to_timeval(int ms, struct timeval *tv) {
    tv->tv_sec=ms/1000; tv->tv_usec=(ms%1000)*1000;
}
static int poll_number;
static int simulated_select(int nfds, fd_set *r, fd_set *w, void *ex, struct timeval *tv) {
    (void)nfds; (void)ex; (void)tv;
    bool watching=FD_ISSET(7,r)&&FD_ISSET(7,w);
    ++poll_number;
    FD_ZERO(r); FD_ZERO(w); // POSIX select clears sets on timeout.
    if(poll_number>=3 && watching) { FD_SET(7,w); return 1; }
    return 0;
}
'''
for name, source in [('before', original), ('after', patched)]:
    code += f'static int {name}(TLS *tls, Config *cfg) {{\n' + preamble(source)
    code += 'return simulated_select(tls->sockfd+1,&tls->rset,&tls->wset,NULL,&tv);\n}\n'
code += r'''
int main(void) {
    Config cfg={2000}; TLS tls={.sockfd=7};
    FD_ZERO(&tls.rset); FD_SET(7,&tls.rset); tls.wset=tls.rset;
    assert(before(&tls,&cfg)==0); assert(before(&tls,&cfg)==0);
    assert(before(&tls,&cfg)==0); // Bug: completed TCP connect is never observed.
    poll_number=0;
    assert(after(&tls,&cfg)==0); assert(after(&tls,&cfg)==0);
    assert(after(&tls,&cfg)==1); // Same slow connection now advances to TLS.
    poll_number=2; assert(after(&tls,&cfg)==1); // Fast connect also works.
    puts("ESP-TLS: repeated poll timeouts retain socket monitoring; slow/fast connect PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='passport-tls-connect-') as temporary:
    source = Path(temporary) / 'connect.c'
    source.write_text(code)
    exe = source.with_suffix('')
    subprocess.run(['cc','-Wall','-Wextra','-Werror',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
