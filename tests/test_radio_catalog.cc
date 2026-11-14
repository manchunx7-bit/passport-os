#include "apps/radio/radio_catalog.h"
#include "apps/radio/radio_json_stream.h"
#include "esp_http_client.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <netdb.h>
#include <string>
#include <vector>

struct TestHttp { std::string url, body; std::size_t offset = 0; bool fail = false; };
std::vector<std::string> queries;
std::string city_fixture, province_fixture, state_fixture, ip_city;
bool fail_dns = false, fail_first = false, fail_all = false, stopped = false, stop_on_read = false, fake_dns = false;
unsigned ip_requests = 0, live_http = 0;
uint32_t esp_random() { return 0; }
extern "C" esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }

std::string decode(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i+2 < s.size()) {
            char hex[3] = {s[i+1],s[i+2],0};out += char(std::strtoul(hex,nullptr,16));i+=2;
        } else out += s[i];
    }
    return out;
}
extern "C" esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    auto *client = new TestHttp;
    ++live_http;
    client->url = decode(config->url);
    client->fail = fail_all || (fail_first && client->url.find("de1.") != std::string::npos);
    if (client->url.find("ip-api") != std::string::npos || client->url.find("ipwho") != std::string::npos) {
        ++ip_requests;
        client->body = "{\"status\":\"success\",\"countryCode\":\"CN\",\"regionName\":\"Guangdong\",\"city\":\""+ip_city+"\"}";
    } else {
        queries.push_back(client->url);
        if (client->url.find("name=广东") != std::string::npos) client->body = province_fixture;
        else if (client->url.find("state=") != std::string::npos) client->body = state_fixture;
        else client->body = city_fixture;
    }
    return client;
}
extern "C" esp_err_t esp_http_client_set_header(esp_http_client_handle_t,const char *,const char *) { return ESP_OK; }
extern "C" esp_err_t esp_http_client_open(esp_http_client_handle_t c,int) { return c->fail ? ESP_FAIL : ESP_OK; }
extern "C" int esp_http_client_fetch_headers(esp_http_client_handle_t) { return 0; }
extern "C" int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
extern "C" int esp_http_client_read(esp_http_client_handle_t c,char *out,int capacity) {
    if (stop_on_read) stopped = true;
    auto n = std::min({std::size_t(capacity),std::size_t(7),c->body.size()-c->offset});
    std::memcpy(out,c->body.data()+c->offset,n);c->offset+=n;return int(n);
}
extern "C" bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->offset==c->body.size(); }
extern "C" esp_err_t esp_http_client_close(esp_http_client_handle_t) { return ESP_OK; }
extern "C" esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { --live_http;delete c;return ESP_OK; }
extern "C" int getaddrinfo(const char *,const char *,const addrinfo *,addrinfo **out) {
    if(fail_dns)return EAI_FAIL;
    static sockaddr_in address;static addrinfo entry;
    address.sin_family=AF_INET;inet_pton(AF_INET,fake_dns?"198.18.1.101":"192.0.2.1",&address.sin_addr);
    entry={};entry.ai_family=AF_INET;entry.ai_addr=reinterpret_cast<sockaddr *>(&address);*out=&entry;
    return 0;
}
extern "C" void freeaddrinfo(addrinfo *) {}
bool cancel() { return stopped; }
std::string station(const std::string &name,const std::string &url,const std::string &extra="") {
    return "{\"name\":\""+name+"\",\"url\":\""+url+"\",\"codec\":\"MP3\""+extra+"}";
}
void reset() {
    assert(live_http==0);queries.clear();ip_requests=0;fail_dns=fail_first=fail_all=stopped=stop_on_read=fake_dns=false;
    ip_city="Zhuhai";city_fixture="[]";province_fixture="[]";state_fixture="[]";
}
void unique(const RadioStation *s,std::size_t n) {
    for(std::size_t i=0;i<n;++i)for(std::size_t j=i+1;j<n;++j)assert(std::strcmp(s[i].url,s[j].url));
}
int main(int argc,char **argv) {
    RadioStation stations[8] = {};RadioLocation preferred={},location={};
    std::strcpy(preferred.city,"珠海");std::strcpy(preferred.region,"广东");
    reset();
    city_fixture="["+station("珠海交通875","http://local/stream",",\"hls\":false,\"lastcheckok\":true")+","+
                      station("珠海先锋951","http://local/2.mp3")+"]";
    province_fixture="[";
    for(int i=0;i<8;++i)province_fixture+=(i?",":"")+station("广东省内"+std::to_string(i),"http://province/"+std::to_string(i));
    province_fixture+="]";
    auto n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==8&&ip_requests==0&&queries.size()==2);
    assert(!std::strcmp(stations[0].name,"珠海交通875")&&stations[0].frequency_decihz==875);
    assert(std::strstr(stations[0].description,"当地")&&std::strstr(stations[2].description,"省内"));unique(stations,n);

    // Voice handover stops after finding its target; browsing still tops up.
    // A missing named station must keep looking in the province directory.
    queries.clear();
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel,"");
    assert(n==8&&queries.size()==1&&!std::strcmp(stations[0].name,"珠海交通875"));
    queries.clear();
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel,"交通");
    assert(n==8&&queries.size()==1);
    queries.clear();
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel,"广东省内");
    assert(n==8&&queries.size()==2);

    reset();
    city_fixture="["+station("一台","http://unique/live")+"]";
    province_fixture="["+station("重复","http://unique/live")+","+station("省台","http://province/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==8&&queries.size()==3&&std::strstr(stations[2].description,"推荐"));unique(stations,n);

    reset();ip_city="Jiangmen";city_fixture="["+station("江门电台","http://jm/live")+"]";
    n=radio_catalog_discover(stations,8,&location,cancel);
    assert(n==7&&ip_requests==1&&!std::strcmp(location.city,"江门"));
    assert(queries[0].find("name=江门")!=std::string::npos);

    reset();fail_dns=true;fail_first=true;city_fixture="["+station("恢复","http://ok/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==7&&queries[0].find("de1.")!=std::string::npos&&queries[1].find("fi1.")!=std::string::npos);

    reset();fake_dns=true;city_fixture="["+station("代理网络","http://proxy/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==7&&queries[0].find("de1.")!=std::string::npos);

    reset();fail_all=true;
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);assert(n==6);unique(stations,n);
    reset();stop_on_read=true;city_fixture="["+station("取消","http://cancel/live")+"]";
    assert(radio_catalog_discover_location(preferred,stations,8,&location,cancel)==0&&live_http==0);
    auto old_queries=queries.size();assert(radio_catalog_discover_location(preferred,stations,8,&location,cancel)==0&&queries.size()==old_queries);

    reset();
    city_fixture="["+station(std::string(9000,'X'),"http://oversize/live")+","+
        station("坏台","http://bad/live",",\"lastcheckok\":false")+","+
        station("HLS台","http://hls/live",",\"hls\":true")+","+
        "{\"name\":\"AAC台\",\"url\":\"http://aac/live.mp3\",\"codec\":\"AAC\"},"+
        station("长URL",std::string("http://url/")+std::string(300,'X'))+","+
        station("有效无后缀","http://valid/live")+"]";
    n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
    assert(n==7&&!std::strcmp(stations[0].name,"有效无后缀")&&live_http==0);

    char object[32];RadioJsonStream parser(object,sizeof(object));std::vector<std::string> objects;
    std::string body="[{\"a\":\"}\\\"{\",\"b\":{}},{\"large\":\""+std::string(100,'x')+"\"},{\"last\":1}]";
    for(char c:body)parser.Feed(&c,1,[&](const char *json){objects.emplace_back(json);});
    assert(parser.Complete()&&objects.size()==2&&objects[1]=="{\"last\":1}");
    RadioJsonStream truncated(object,sizeof(object));truncated.Feed("[{",2,[](const char *){});assert(!truncated.Complete());
    RadioJsonStream invalid(object,sizeof(object));invalid.Feed("[{},]",5,[](const char *){});assert(!invalid.Complete());
    if(argc==3) {
        reset();
        std::ifstream local(argv[1]),province(argv[2]);assert(local&&province);
        city_fixture.assign(std::istreambuf_iterator<char>(local),{});
        province_fixture.assign(std::istreambuf_iterator<char>(province),{});
        n=radio_catalog_discover_location(preferred,stations,8,&location,cancel);
        assert(n==8&&std::strstr(stations[0].name,"珠海")&&std::strstr(stations[1].name,"珠海"));
        unique(stations,n);
        std::puts("Live directory fixtures through production parser: 2 Zhuhai + 6 province stations");
        for(std::size_t i=0;i<n;++i)std::printf("%zu: %s | %s\n",i+1,stations[i].name,stations[i].description);
    }
    std::puts("Radio: shared city, IP translation, sparse top-up/dedup, mirror failover, cancellation, MP3 metadata and bounded streamed JSON: PASS");
}
