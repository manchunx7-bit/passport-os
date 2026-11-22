"""Exercise production base64url decoding with real mbedTLS and shared storage."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'components/passport_muse/sdk/link_pairing.c').read_text()
decode = source[source.index('static bool base64url_decode('):
                source.index('static bool json_string(')]
mbedtls = Path(os.environ.get('IDF_PATH', '/opt/esp-idf')) / 'components/mbedtls/mbedtls'
prelude = r'''
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "bsp_audio_workspace.h"
#include "muse_pairing_memory.h"
// Any reintroduced heap staging must fail compilation.
#define malloc pairing_decode_must_not_allocate
#define free pairing_decode_must_not_free
'''
checks = r'''
#undef malloc
#undef free
using namespace audio_workspace;
static void run(const std::string &input, const std::string &expected,
                bool success=true, size_t capacity=3075) {
    auto *scratch=muse_pairing_decode_buffer(4097); assert(scratch);
    memset(scratch,0x7a,4097);
    uint8_t output[3077]; memset(output,0x5a,sizeof(output));
    size_t length=9999;
    bool result=base64url_decode(input.c_str(),output,capacity,&length);
    assert(result==success);
    if(success) {
        assert(length==expected.size());
        assert(!memcmp(output,expected.data(),length));
    } else assert(length==9999);
    assert(output[capacity]==0x5a && output[3076]==0x5a);
    const size_t n=input.size();
    const size_t used=n && n<=4096 && n%4!=1 ? (n+3)/4*4+1 : 0;
    for(size_t i=0;i<4097;++i) assert(scratch[i]==(i<used?0:0x7a));
    for(size_t i=0;i<8192+3075;++i) assert(storage[i]==0xa5);
}
int main() {
    uint8_t out[4]; size_t length=123;
    assert(!base64url_decode("Zm9v",out,sizeof(out),&length));
    assert(length==123); // No active pairing workspace.
    assert(acquire(Owner::Radio));
    assert(!base64url_decode("Zm9v",out,sizeof(out),&length));
    assert(release(Owner::Radio));
    assert(muse_pairing_acquire_rx(8192));
    memset(storage,0xa5,8192+3075);
    assert(!base64url_decode(nullptr,out,4,&length));
    assert(!base64url_decode("Zg",nullptr,4,&length));
    assert(!base64url_decode("Zg",out,4,nullptr));
    run("Zg","f"); run("Zm8","fo"); run("Zm9v","foo");
    run("Zm9vYg","foob"); run("Zm9vYmE","fooba"); run("Zm9vYmFy","foobar");
    run("-_8",std::string("\xfb\xff",2));
    run("","",false); run("a","",false); run(std::string(4097,'A'),"",false);
    for(auto input:{"Zg==","Zm8=","Zm+v","Zm/v","Zm v","Zm\nv","Zm?8","Zm.8"})
        run(input,"",false);
    run(std::string("Z")+char(0xff),"",false);
    run("Zm9v","",false,2); run("Zg","",false,0);
    for(unsigned i=0;i<1000;++i) {
        run(std::string(4096,'A'),std::string(3072,'\0'));
        run(std::string(4095,'A'),std::string(3071,'\0'));
        run(std::string(4094,'A'),std::string(3070,'\0'));
    }
    muse_pairing_release_rx();
    for(size_t i=0;i<15364;++i)assert(!storage[i]);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    (directory / 'pairing_test_config.h').write_text('#define MBEDTLS_BASE64_C\n')
    flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g',
             '-DMBEDTLS_CONFIG_FILE="pairing_test_config.h"',
             '-I' + str(directory), '-I' + str(mbedtls / 'include')]
    objects = []
    for name in ('base64', 'platform_util', 'constant_time'):
        obj = directory / (name + '.o')
        subprocess.run(['cc', '-std=c11', *flags, '-c',
                        str(mbedtls / 'library' / (name + '.c')), '-o', str(obj)], check=True)
        objects.append(str(obj))
    test = directory / 'test.cpp'
    test.write_text(prelude + decode + checks)
    binary = directory / 'test'
    subprocess.run(['c++', '-std=c++17', *flags,
                    '-I' + str(ROOT / 'components/bsp/include'),
                    '-I' + str(ROOT / 'components/passport_muse/sdk'),
                    str(ROOT / 'components/passport_muse/sdk/muse_pairing_memory.cpp'),
                    str(test), *objects, '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('Pairing base64: real mbedTLS, vectors/bounds/OOM/erasure, 3000 max-size decodes: PASS')
