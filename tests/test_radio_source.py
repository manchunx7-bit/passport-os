"""Exercise production HTTP/HLS transport against deterministic network fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    'esp_http_client.h': r'''
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
constexpr int ESP_OK=0,HTTP_EVENT_ON_HEADER=1;
struct Http;
using esp_http_client_handle_t=Http*;
struct esp_http_client_event_t {int event_id;void* user_data;char* header_key;char* header_value;};
struct esp_http_client_config_t {const char* url;int timeout_ms,buffer_size,buffer_size_tx;const char* user_agent;int(*crt_bundle_attach)(void*);int(*event_handler)(esp_http_client_event_t*);void* user_data;bool keep_alive_enable;};
Http* esp_http_client_init(const esp_http_client_config_t*);
int esp_http_client_open(Http*,int);
int esp_http_client_fetch_headers(Http*);
int esp_http_client_get_status_code(Http*);
int esp_http_client_set_redirection(Http*);
void esp_http_client_set_header(Http*,const char*,const char*);
void esp_http_client_close(Http*);
void esp_http_client_cleanup(Http*);
int esp_http_client_get_url(Http*,char*,size_t);
int esp_http_client_read(Http*,char*,size_t);
bool esp_http_client_is_complete_data_received(Http*);
''',
    'simple_dec/esp_audio_simple_dec.h': '#pragma once\nenum esp_audio_simple_dec_type_t {ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,ESP_AUDIO_SIMPLE_DEC_TYPE_AAC,ESP_AUDIO_SIMPLE_DEC_TYPE_TS};\n',
    'esp_crt_bundle.h': '#pragma once\ninline int esp_crt_bundle_attach(void*) {return 0;}\n',
    'esp_log.h': '#pragma once\n#include <cstdio>\n#define ESP_LOGW(tag,...) do {(void)tag; printf(__VA_ARGS__); puts("");} while(0)\n#define ESP_LOGI ESP_LOGW\n',
    'esp_timer.h': '#pragma once\n#include <cstdint>\nint64_t esp_timer_get_time();\n',
    'freertos/FreeRTOS.h': '#pragma once\n#define pdMS_TO_TICKS(x) (x)\n',
    'freertos/task.h': '#pragma once\nvoid vTaskDelay(unsigned);\n',
}
TEST = r'''
#include "apps/radio/radio_source.h"
#include "apps/podcast/podcast_catalog.h"
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
struct Response {std::string body,mime="application/vnd.apple.mpegurl",redirect;int status=200;bool truncated=false;};
struct Http {esp_http_client_config_t cfg;std::string url;Response response;size_t offset=0;};
static std::map<std::string,std::vector<Response>> responses;
static std::map<std::string,unsigned> requests;
static unsigned live=0,peak=0,waits=0;
static int64_t clock_us=0;
static bool cancelled=false,cancel_wait=false;
static std::string received_range;
Http* esp_http_client_init(const esp_http_client_config_t*c){auto*p=new Http; p->cfg=*c;p->url=c->url;++live;peak=std::max(peak,live);return p;}
int esp_http_client_open(Http*p,int){auto i=responses.find(p->url);if(i==responses.end())return -1;auto n=requests[p->url]++;p->response=i->second[std::min<size_t>(n,i->second.size()-1)];p->offset=0;return 0;}
int esp_http_client_fetch_headers(Http*p){char key[]="Content-Type";esp_http_client_event_t e{HTTP_EVENT_ON_HEADER,p->cfg.user_data,key,p->response.mime.data()};if(p->cfg.event_handler)p->cfg.event_handler(&e);return int(p->response.body.size());}
int esp_http_client_get_status_code(Http*p){return p->response.status;}
int esp_http_client_set_redirection(Http*p){p->url=p->response.redirect;return 0;}
void esp_http_client_set_header(Http*,const char* key,const char* value){if(!strcmp(key,"Range"))received_range=value;}
void esp_http_client_close(Http*){}
void esp_http_client_cleanup(Http*p){assert(live);--live;delete p;}
int esp_http_client_get_url(Http*p,char*out,size_t size){if(p->url.size()>=size)return -1;strcpy(out,p->url.c_str());return 0;}
int esp_http_client_read(Http*p,char*out,size_t size){auto n=std::min({size,p->response.body.size()-p->offset,size_t(73)});memcpy(out,p->response.body.data()+p->offset,n);p->offset+=n;return int(n);}
bool esp_http_client_is_complete_data_received(Http*p){return p->offset==p->response.body.size()&&!p->response.truncated;}
int64_t esp_timer_get_time(){return clock_us;}
void vTaskDelay(unsigned ms){clock_us+=ms*1000;++waits;if(cancel_wait)cancelled=true;}
bool stop(void*){return cancelled;}
std::string adts(char payload){std::string s(1000,payload);s[0]=char(0xff);s[1]=char(0xf1);return s;}
void reset(){assert(!live);received_range.clear();responses.clear();requests.clear();clock_us=0;waits=0;cancelled=cancel_wait=false;}
std::string drain(RadioSource&s){std::string out;uint8_t data[127];for(unsigned i=0;i<1000;++i){int n=s.read(data,sizeof(data));if(n<=0)return out;out.append((char*)data,n);}assert(false);return {};}
int main(){
 reset();responses["http://r/mp3"]={{std::string("\xff\xfb")+std::string(2000,'m'),"audio/mpeg"}};
 {RadioSource s(stop,nullptr);assert(s.open("http://r/mp3")&&s.type()==ESP_AUDIO_SIMPLE_DEC_TYPE_MP3);assert(drain(s)==responses["http://r/mp3"][0].body&&s.complete());}
 reset();responses["http://r/wrong.mp3"]={{adts('a'),"audio/aacp"}};
 {RadioSource s(stop,nullptr);assert(s.open("http://r/wrong.mp3")&&s.type()==ESP_AUDIO_SIMPLE_DEC_TYPE_AAC);assert(drain(s)==adts('a'));}
 reset();
 responses["http://r/start.m3u8"]={{"","", "https://cdn.test/live/master.m3u8",302}};
 responses["https://cdn.test/live/master.m3u8"]={{"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\"\nmedia.m3u8\n"}};
 responses["https://cdn.test/live/media.m3u8"]={{"#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:10\n#EXTINF:6,\na.aac\n#EXTINF:6,\nb.aac\n#EXT-X-ENDLIST\n"}};
 responses["https://cdn.test/live/a.aac"]={{adts('a'),"audio/aac"}};responses["https://cdn.test/live/b.aac"]={{adts('b'),"audio/aac"}};
 alignas(16) unsigned char workspace[RadioSource::kHlsWorkspaceBytes+32];memset(workspace,0x5a,sizeof(workspace));
 {RadioSource s(stop,nullptr,workspace+16,RadioSource::kHlsWorkspaceBytes);assert(s.open("http://r/start.m3u8"));assert(drain(s)==adts('a')+adts('b')&&s.complete());assert(waits==0);}
 for(unsigned i=0;i<16;++i){assert(workspace[i]==0x5a);assert(workspace[sizeof(workspace)-1-i]==0x5a);}
 assert(peak==1);
 reset();
 responses["http://r/live.m3u8"]={
 {"#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:20\n#EXTINF:6,\na.ts\n"},
 {"#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:20\n#EXTINF:6,\na.ts\n#EXTINF:6,\nb.ts\n#EXT-X-ENDLIST\n"}};
 std::string ts(1000,'t');ts[0]=ts[188]=ts[376]=0x47;
 responses["http://r/a.ts"]={{ts,"video/mp2t"}};responses["http://r/b.ts"]={{ts,"video/mp2t"}};
 {RadioSource s(stop,nullptr);assert(s.open("http://r/live.m3u8")&&s.type()==ESP_AUDIO_SIMPLE_DEC_TYPE_TS);assert(drain(s)==ts+ts);assert(requests["http://r/a.ts"]==1&&requests["http://r/b.ts"]==1&&waits==0);}
 reset();responses["http://r/live.m3u8"]={{"#EXTM3U\n#EXTINF:6,\na.aac\n"}};responses["http://r/a.aac"]={{adts('a'),"audio/aac"}};
 {RadioSource s(stop,nullptr);assert(s.open("http://r/live.m3u8"));cancel_wait=true;assert(drain(s)==adts('a'));assert(cancelled&&waits==1);}
 reset();responses["http://r/disc.m3u8"]={{"#EXTM3U\n#EXTINF:6,\na.aac\n#EXT-X-DISCONTINUITY\n#EXTINF:6,\nb.aac\n#EXT-X-ENDLIST\n"}};responses["http://r/a.aac"]={{adts('a'),"audio/aac"}};
 {RadioSource s(stop,nullptr);assert(s.open("http://r/disc.m3u8"));assert(drain(s)==adts('a'));assert(!requests["http://r/b.aac"]);}
 for(const auto& body:{std::string("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n#EXTINF:6,\na.ts\n"),std::string("#EXTM3U\n")+std::string(2000,'x'),std::string("<html>error</html>")}){
 reset();responses["http://r/bad.m3u8"]={{body}};{RadioSource s(stop,nullptr);assert(!s.open("http://r/bad.m3u8"));}}
 reset();responses["http://r/trunc.m3u8"]={{"#EXTM3U\n#EXTINF:6,\na.ts\n","application/vnd.apple.mpegurl","",200,true}};
 {RadioSource s(stop,nullptr);assert(!s.open("http://r/trunc.m3u8"));}
 reset();
 responses["https://r/episode.mp3"]={{std::string(1800,'m'),"audio/mpeg","",206,false}};
 {RadioSource s(stop,nullptr);assert(s.open("https://r/episode.mp3",1234)&&s.status()==206);
  assert(received_range=="bytes=1234-"&&drain(s).size()==1800&&s.complete());}
 reset();
 responses["https://r/episode.mp3"]={{std::string(1800,'m'),"audio/mpeg","",200,true}};
 {RadioSource s(stop,nullptr);assert(s.open("https://r/episode.mp3",1234)&&s.status()==200);
  assert(drain(s).size()==1800&&!s.complete());}
 reset();
 const std::string api="http://mobile.ximalaya.com/mobile/others/ca/album/track/51076156/true/2/3";
 const std::string list=R"json({"album":{"tracks":1084,"list":[]},"tracks":{"list":[
 {"trackId":42,"title":"Episode \"42\" with {braces}","duration":120,"isPaid":false,
  "playUrl64":"http://bad.example/x.mp3","playUrl32":"http://a.xmcdn.com/a.mp3"},
 {"trackId":43,"title":"Paid","isPaid":true,"playUrl64":"http://a.xmcdn.com/b.mp3"},
 {"trackId":44,"title":"Two encodes","playUrl64":"https://a.xmcdn.com/hq.mp3","playUrl32":"http://a.xmcdn.com/lq.mp3"}],"totalCount":50}})json";
 responses[api]={{list,"application/json"}};
 std::atomic<bool> abort=false;
 podcast::Page page{};
 assert(podcast::fetch_page(51076156,2,abort,page));
 assert(page.count==3&&page.total==50&&page.episodes[0].duration==120);
 assert(!strcmp(page.episodes[0].title,"Episode \"42\" with {braces}"));
 assert(!strcmp(page.episodes[0].url,"http://a.xmcdn.com/a.mp3"));
 assert(page.episodes[1].paid&&!*page.episodes[1].url&&!*page.episodes[1].backup_url);
 assert(!strcmp(page.episodes[2].url,"http://a.xmcdn.com/hq.mp3"));
 assert(!strcmp(page.episodes[2].backup_url,"http://a.xmcdn.com/lq.mp3"));
 abort=true;assert(!podcast::fetch_page(51076156,2,abort,page)&&requests[api]==1);
 abort=false;responses[api][0].truncated=true;
 assert(!podcast::fetch_page(51076156,2,abort,page));
 char url[48]{};
 assert(!podcast::audio_url("http://a.xmcdn.com@evil.invalid/a.mp3",url,sizeof(url)));
 assert(!podcast::audio_url("http://notxmcdn.com/a.mp3",url,sizeof(url)));
 assert(!podcast::audio_url("http://a.xmcdn.com/a.mp3",url,8));
 assert(podcast::audio_url("https://a.xmcdn.com/s.mp3",url,sizeof(url))&&!strcmp(url,"http://a.xmcdn.com/s.mp3"));
 char exact[25]{};
 assert(!podcast::audio_url("http://a.xmcdn.com/a.mp3",exact,24));
 assert(podcast::audio_url("http://a.xmcdn.com/a.mp3",exact,25)&&!strcmp(exact,"http://a.xmcdn.com/a.mp3"));
 const std::string search="http://search.ximalaya.com/front/v1?kw=%E6%92%AD%E5%AE%A2&core=album&page=1&rows=8";
 const std::string programs=R"json({"response":{"docs":[
 {"id":1,"title":"Studio FM Extra","play":1,"tracks":3},
 {"id":2,"title":"Studio FM","play":9,"tracks":3},
 {"id":2,"title":"Duplicate","play":10,"tracks":3},
 {"id":3,"title":"Paid","is_paid":true,"play":200,"tracks":3},
 {"id":4,"title":"Draft","isDraft":true,"tracks":3},
 {"id":5,"title":"Empty","tracks":0}]}})json";
 responses[search]={{programs,"application/json"}};
 podcast::ProgramList results{};
 assert(podcast::discover(podcast::Discovery::Search,"播客",abort,results));
 assert(results.count==2&&results.items[0].id==1&&results.items[1].id==2);
 assert(results.items[0].tracks==3&&results.items[1].tracks==3);
 assert(podcast::match_program(results,"studio fm")==1);
 assert(podcast::match_program(results,"no match")==results.count);
 assert(podcast::match_program(results,"Studio")==results.count);
 assert(podcast::match_book(results,"Studio")==1);
 assert(podcast::match_book(results,"studio fm")==1);
 assert(podcast::match_book(results,"no match")==results.count);
 assert(podcast::discover(podcast::Discovery::Hot,nullptr,abort,results)&&results.items[0].id==2);
 const std::string encoded="http://search.ximalaya.com/front/v1?kw=A%26%20B&core=album&page=1&rows=8";
 responses[encoded]={{programs,"application/json"}};
 assert(podcast::discover(podcast::Discovery::Search,"A& B",abort,results)&&requests[encoded]==1);
 const char *theme = podcast::recommended_theme_name();
 assert(theme && *theme);
 for (const auto &[src, kw] : std::vector<std::pair<podcast::Discovery, std::string>>{
         {podcast::Discovery::Business, "%E5%95%86%E4%B8%9A%20%E8%B4%A2%E7%BB%8F"},
         {podcast::Discovery::Story, "%E6%95%85%E4%BA%8B%20%E4%BA%BA%E6%96%87"},
         {podcast::Discovery::Crosstalk, "%E7%9B%B8%E5%A3%B0%20%E8%AF%84%E4%B9%A6"},
         {podcast::Discovery::BookHot, "%E6%9C%89%E5%A3%B0%E4%B9%A6"},
         {podcast::Discovery::BookMystery, "%E6%82%AC%E7%96%91%20%E6%9C%89%E5%A3%B0%E4%B9%A6"},
         {podcast::Discovery::BookHistory, "%E5%8E%86%E5%8F%B2%20%E8%AF%84%E4%B9%A6"}}) {
     const std::string ch_url = "http://search.ximalaya.com/front/v1?kw=" + kw + "&core=album&page=1&rows=8";
     responses[ch_url] = {{programs, "application/json"}};
     assert(podcast::discover(src, nullptr, abort, results) && results.count == 2 && results.items[0].id == 2);
 }
 const std::string classics_url = "http://search.ximalaya.com/front/v1?kw=%E4%B8%96%E7%95%8C%E5%90%8D%E8%91%97&core=album&page=1&rows=8";
 const std::string scifi_url = "http://search.ximalaya.com/front/v1?kw=%E7%A7%91%E5%B9%BB%20%E6%9C%89%E5%A3%B0%E4%B9%A6&core=album&page=1&rows=8";
 const std::string scifi_programs = R"json({"response":{"docs":[
 {"id":2,"title":"Duplicate Classic","play":999,"tracks":50},
 {"id":9,"title":"Three Body","play":50,"tracks":120}]}})json";
 responses[classics_url] = {{programs, "application/json"}};
 responses[scifi_url] = {{scifi_programs, "application/json"}};
 assert(podcast::discover(podcast::Discovery::BookClassics, nullptr, abort, results) &&
        results.count == 3 && results.items[0].id == 2 && results.items[1].id == 1 && results.items[2].id == 9);
 abort=true;assert(!podcast::discover(podcast::Discovery::Hot,nullptr,abort,results));
 puts("Podcast production source: fragmented catalog, pagination, filtered discovery, channels, escaped search, title matching, popularity order, paid entries, HTTP URL normalization/validation, cancellation, Range and EOF PASS");
 assert(live==0);puts("Production HLS transport: redirects, segments, no replay, refresh, cancellation, cleanup, invalid bodies PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='radio-source-') as temp:
    tmp = Path(temp)
    for name, source in HEADERS.items():
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
    (tmp / 'test.cc').write_text(TEST)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-fsanitize=address,undefined',
                    '-I'+str(tmp), '-I'+str(ROOT/'main'), '-I'+str(ROOT/'tests/vendor/cjson'), str(tmp/'test.cc'),
                    str(ROOT/'main/apps/radio/radio_source.cc'), str(ROOT/'main/apps/podcast/podcast_catalog.cc'),
                    str(ROOT/'tests/vendor/cjson/cJSON.c'), '-o', str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test')], check=True)
