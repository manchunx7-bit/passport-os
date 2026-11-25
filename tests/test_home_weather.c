#include "apps/home/home_profile.h"
#include "apps/home/home_weather.h"
#include "esp_http_client.h"
#include "esp_spiffs.h"
#include "esp_partition.h"
#include "nvs.h"
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char key[20],text[160];int value;bool used; } Slot;
static Slot stored[24],working[24];
static bool fail_commit,fail_http,switch_city;
static char last_url[400];
static unsigned http_count;
static unsigned http_active;
static bool fail_air_http;
static int air_http_status=200;
static int64_t now_us;
static bool stalled_request,cancel_on_delay,incomplete_body;
static const char *stall_host;
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ticks) { now_us+=(int64_t)ticks*1000;if(cancel_on_delay){cancel_on_delay=false;home_weather_cancel_requests();} }
static home_profile_t zhuhai;
static Slot *slot(Slot *slots,const char *key,bool create) {
    for(int i=0;i<24;i++)if(slots[i].used&&!strcmp(slots[i].key,key))return &slots[i];
    if(create)for(int i=0;i<24;i++)if(!slots[i].used){slots[i].used=true;snprintf(slots[i].key,sizeof(slots[i].key),"%s",key);return &slots[i];}
    return NULL;
}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h) { (void)name;*h=mode;if(mode==NVS_READWRITE)memcpy(working,stored,sizeof(stored));return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;if(fail_commit)return ESP_FAIL;memcpy(stored,working,sizeof(stored));return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *out,size_t *n) {
    (void)h;Slot *s=slot(stored,key,false);if(!s)return ESP_ERR_NOT_FOUND;
    size_t need=strlen(s->text)+1;if(*n<need)return ESP_ERR_INVALID_SIZE;memcpy(out,s->text,need);*n=need;return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h,const char *key,const char *value) { (void)h;Slot *s=slot(working,key,true);assert(s);snprintf(s->text,sizeof(s->text),"%s",value);return ESP_OK; }
#define NVS_NUMBER(type,suffix) \
esp_err_t nvs_get_##suffix(nvs_handle_t h,const char *key,type *out) { (void)h;Slot *s=slot(stored,key,false);if(!s)return ESP_ERR_NOT_FOUND;*out=(type)s->value;return ESP_OK; } \
esp_err_t nvs_set_##suffix(nvs_handle_t h,const char *key,type value) { (void)h;Slot *s=slot(working,key,true);assert(s);s->value=value;return ESP_OK; }
NVS_NUMBER(int16_t,i16)
NVS_NUMBER(uint8_t,u8)
NVS_NUMBER(uint16_t,u16)
NVS_NUMBER(uint32_t,u32)
static int tz_minutes=480,tz_writes;
esp_err_t time_sync_set_tz_offset_min(int minutes) { if(minutes!=tz_minutes){tz_minutes=minutes;++tz_writes;}return ESP_OK; }
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c) { (void)c;return ESP_FAIL; }
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label) { (void)type;(void)subtype;(void)label;return NULL; }
esp_err_t esp_partition_read(const esp_partition_t *p,size_t offset,void *out,size_t size) { (void)p;(void)offset;(void)out;(void)size;assert(!"no storage partition in weather fixture");return ESP_FAIL; }
esp_err_t esp_spiffs_info(const char *label,size_t *total,size_t *used) { (void)label;*total=2*1024*1024;*used=0;return ESP_OK; }
esp_err_t esp_crt_bundle_attach(void *p) { (void)p;return ESP_OK; }
struct TestHttp { size_t offset; const char *body; bool air,connected; esp_http_client_config_t config; };
static const char response[]="{\"utc_offset_seconds\":28800,\"current\":{\"temperature_2m\":26.1,\"weather_code\":2,\"relative_humidity_2m\":77,\"wind_speed_10m\":11,\"wind_direction_10m\":180}}";
static const char *weather_response=response;
static const char good_air[]="{\"current_units\":{\"pm2_5\":\"μg/m³\"},\"current\":{\"pm2_5\":56.1}}";
static const char *air_response=good_air;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    ++http_count;snprintf(last_url,sizeof(last_url),"%s",c->url);
    assert(http_active==0);++http_active;
    struct TestHttp *client=calloc(1,sizeof(*client));assert(client);
    assert(c->is_async&&c->timeout_ms==2000);client->config=*c;
    client->air=strstr(c->url,"/v1/air-quality")!=NULL;
    client->body=client->air?air_response:weather_response;return client;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->air?air_http_status:200; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    if(fail_http||(c->air&&fail_air_http))return ESP_FAIL;
    if(stalled_request||(stall_host&&strstr(c->config.url,stall_host)))return ESP_ERR_HTTP_EAGAIN;
    if(!c->connected){
        c->connected=true;esp_http_client_event_t connected={.event_id=HTTP_EVENT_ON_CONNECTED,.user_data=c->config.user_data,.client=c};
        assert(c->config.event_handler(&connected)==ESP_OK);assert(c->config.timeout_ms==500);
    }
    if(switch_city){switch_city=false;assert(home_profile_set_text(&zhuhai)==ESP_OK);}
    size_t n=strlen(c->body)-c->offset;if(n>9)n=9;
    esp_http_client_event_t event={HTTP_EVENT_ON_DATA,(void *)(c->body+c->offset),(int)n,c->config.user_data,c};
    c->offset+=n;esp_err_t err=c->config.event_handler(&event);if(err!=ESP_OK)return err;
    return c->offset==strlen(c->body)?ESP_OK:ESP_ERR_HTTP_EAGAIN;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return !incomplete_body&&c->offset==strlen(c->body); }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c,int timeout) { c->config.timeout_ms=timeout;return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { assert(http_active==1);--http_active;free(c);return ESP_OK; }

static void *json_allocate(size_t n) { assert(http_active==0);return malloc(n); }

int main(int argc,char **argv) {
    cJSON_Hooks hooks={json_allocate,free};cJSON_InitHooks(&hooks);
    assert(home_profile_coordinates_valid("22.2707","113.5767"));
    assert(home_profile_coordinates_valid("", ""));
    assert(home_profile_coordinates_valid("-90","180"));
    for(const char **p=(const char *[]){"nan","inf","91","22&bad=1","1e2","--2",".",NULL};*p;++p)
        assert(!home_profile_coordinates_valid(*p,"113.6"));
    assert(!home_profile_coordinates_valid("22",""));
    home_profile_init();home_weather_init();home_weather_t weather;assert(!home_weather_get(&weather));
    strcpy(zhuhai.city,"珠海");strcpy(zhuhai.region,"广东");strcpy(zhuhai.radio_city,"珠海");strcpy(zhuhai.country_code,"CN");
    strcpy(zhuhai.lat,"22.2707");strcpy(zhuhai.lon,"113.5767");
    assert(home_profile_set_text(&zhuhai)==ESP_OK);
    assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);
    assert(strstr(last_url,"wind_direction_10m&timezone=auto"));
    assert(home_weather_get(&weather)&&weather.temp_c==26&&weather.humidity==77);
    // Real clock in the host test: a fresh fetch is stamped and persisted.
    assert(weather.updated_at>=1700000000u&&tz_minutes==480&&tz_writes==0);
    {
        const uint32_t stamp=weather.updated_at;
        home_weather_init();assert(home_weather_get(&weather)&&weather.updated_at==stamp);
        char age[24];
        home_weather_age_text(stamp,stamp+30,age,sizeof(age));assert(!strcmp(age,"刚刚"));
        home_weather_age_text(stamp,stamp+25*60,age,sizeof(age));assert(!strcmp(age,"25分钟前"));
        home_weather_age_text(stamp,stamp+3*3600+5,age,sizeof(age));assert(!strcmp(age,"3小时前"));
        home_weather_age_text(stamp,stamp+2*86400,age,sizeof(age));assert(!strcmp(age,"2天前"));
        home_weather_age_text(0,stamp,age,sizeof(age));assert(!strcmp(age,"离线数据"));
        home_weather_age_text(stamp,0,age,sizeof(age));assert(!strcmp(age,"离线数据"));
        home_weather_age_text(stamp,stamp-100,age,sizeof(age));assert(!strcmp(age,"刚刚"));
    }
    // The device follows the city's UTC offset (India +5:30); junk offsets are ignored.
    weather_response="{\"utc_offset_seconds\":19800,\"current\":{\"temperature_2m\":30,\"weather_code\":0,\"relative_humidity_2m\":50,\"wind_speed_10m\":5,\"wind_direction_10m\":90}}";
    assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK&&tz_minutes==330&&tz_writes==1);
    for(const char **bad=(const char *[]){"1e9","123","\"28800\"","-50000",NULL};*bad;++bad){
        char body[256];
        snprintf(body,sizeof(body),"{\"utc_offset_seconds\":%s,\"current\":{\"temperature_2m\":30,\"weather_code\":0,\"relative_humidity_2m\":50,\"wind_speed_10m\":5,\"wind_direction_10m\":90}}",*bad);
        weather_response=body;assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK&&tz_minutes==330);
    }
    weather_response=response;assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK&&tz_minutes==480&&tz_writes==2);
    for(const char **bad=(const char *[]){"{}",
        "{\"current\":{\"temperature_2m\":1e308,\"weather_code\":2}}",
        "{\"current\":{\"temperature_2m\":26,\"weather_code\":2,\"relative_humidity_2m\":null,\"wind_speed_10m\":11,\"wind_direction_10m\":180}}",
        "{\"current\":{\"temperature_2m\":26,\"weather_code\":2,\"relative_humidity_2m\":120,\"wind_speed_10m\":11,\"wind_direction_10m\":180}}",NULL};*bad;++bad){
        weather_response=*bad;assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);
        assert(home_weather_get(&weather)&&weather.temp_c==26&&weather.humidity==77);
    }
    weather_response=response;
    home_profile_t beijing=zhuhai;strcpy(beijing.city,"北京");strcpy(beijing.region,"北京");strcpy(beijing.radio_city,"北京");
    strcpy(beijing.lat,"39.9042");strcpy(beijing.lon,"116.4074");
    fail_commit=true;assert(home_profile_set_text(&beijing)==ESP_FAIL);assert(home_weather_get(&weather));
    fail_commit=false;assert(home_profile_set_text(&beijing)==ESP_OK);assert(!home_weather_get(&weather)&&!weather.valid);
    fail_http=true;assert(home_weather_refresh(beijing.lat,beijing.lon)==ESP_FAIL);assert(!home_weather_get(&weather));fail_http=false;
    assert(home_weather_refresh(beijing.lat,beijing.lon)==ESP_OK);assert(home_weather_get(&weather));
    assert(home_profile_set_text(&zhuhai)==ESP_OK);home_weather_init();assert(!home_weather_get(&weather));
    assert(home_profile_set_text(&beijing)==ESP_OK);switch_city=true;
    assert(home_weather_refresh(beijing.lat,beijing.lon)==ESP_OK);assert(!home_weather_get(&weather));
    unsigned before=http_count;assert(home_weather_refresh("22&city=evil","113.6")==ESP_ERR_INVALID_ARG);assert(http_count==before);
    home_profile_t invalid=zhuhai;strcpy(invalid.lat,"91");assert(home_profile_set_text(&invalid)==ESP_ERR_INVALID_ARG);
    // Old NVS profiles omit the added metadata, and still load existing city/coords.
    memset(stored,0,sizeof(stored));
    nvs_handle_t h;nvs_open("home_profile",NVS_READWRITE,&h);nvs_set_str(h,"city","珠海");nvs_set_str(h,"lat","22.2707");nvs_set_str(h,"lon","113.5767");nvs_commit(h);
    home_profile_init();home_profile_get(&invalid);assert(!strcmp(invalid.city,"珠海")&&!invalid.region[0]&&!invalid.country_code[0]);
    // PM2.5 uses the same location, with independent validity and no persistent cache.
    home_air_quality_t air;char label[32];assert(!home_air_quality_get(&air));
    assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);
    assert(strstr(last_url,"air-quality-api.open-meteo.com/v1/air-quality"));
    assert(strstr(last_url,"current=pm2_5&timezone=auto")&&!strstr(last_url,"aqi"));
    assert(home_air_quality_get(&air)&&air.pm25_x10==561);
    // Leaving and returning to Home keeps a fresh sample without HTTP; expiry
    // is measured by uptime and does not depend on SNTP or phone clock changes.
    {
        const unsigned requests=http_count;
        const int64_t sampled=now_us;
        home_weather_cancel_requests();home_weather_allow_requests();
        assert(home_air_quality_get(&air)&&air.pm25_x10==561&&http_count==requests);
        now_us=sampled+1800000000LL-1;assert(home_air_quality_get(&air));
        now_us=sampled+1800000000LL;assert(!home_air_quality_get(&air));
        assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);
        assert(home_air_quality_get(&air));
    }
    home_air_quality_label(true,label,sizeof(label));assert(!strcmp(label,"PM2.5 56.1"));
    home_air_quality_label(false,label,sizeof(label));assert(!strcmp(label,"PM2.5 --"));
    assert(home_profile_set_text(&beijing)==ESP_OK);assert(!home_air_quality_get(&air));
    assert(home_profile_set_text(&zhuhai)==ESP_OK);
    fail_air_http=true;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(!home_air_quality_get(&air));fail_air_http=false;
    air_http_status=429;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(!home_air_quality_get(&air));air_http_status=200;
    for(const char **bad=(const char *[]){
        "{}", "{\"current\":{\"pm2_5\":12}}",
        "{\"current_units\":{\"pm2_5\":\"μg/m³\"},\"current\":{\"pm2_5\":null}}",
        "{\"current_units\":{\"pm2_5\":\"μg/m³\"},\"current\":{\"pm2_5\":-1}}",
        "{\"current_units\":{\"pm2_5\":\"μg/m³\"},\"current\":{\"pm2_5\":1e308}}",
        "{\"current_units\":{\"pm2_5\":\"mg/m³\"},\"current\":{\"pm2_5\":12}}",NULL};*bad;++bad){
        air_response=*bad;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(!home_air_quality_get(&air));
    }
    char huge[1300];memset(huge,' ',sizeof(huge));memcpy(huge,good_air,strlen(good_air));huge[sizeof(huge)-1]=0;
    air_response=huge;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(!home_air_quality_get(&air));
    air_response="{\"current_units\":{\"pm2_5\":\"μg/m³\"},\"current\":{\"pm2_5\":0}}";
    assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);home_air_quality_label(true,label,sizeof(label));assert(!strcmp(label,"PM2.5 0.0"));
    air_response="{\"current_units\":{\"pm2_5\":\"μg/m³\"},\"current\":{\"pm2_5\":10000}}";
    assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);home_air_quality_label(true,label,sizeof(label));assert(!strcmp(label,"PM2.5 10000"));
    char tiny[4];home_air_quality_label(true,tiny,sizeof(tiny));assert(tiny[3]==0);
    air_response=good_air;
    incomplete_body=true;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(!home_air_quality_get(&air));incomplete_body=false;
    stalled_request=true;int64_t start=now_us;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(now_us-start==30000000&&http_active==0);
    cancel_on_delay=true;start=now_us;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(now_us-start==20000&&http_active==0);
    before=http_count;assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);assert(http_count==before && "cancelled weather must not create a client");
    home_weather_allow_requests();stalled_request=false;
    // A geo-DNS weather node that never connects must not hide the weather:
    // each host gets half of the budget, and the working host is tried first next time.
    stall_host="//api.open-meteo.com/";before=http_count;start=now_us;
    assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);
    assert(http_count==before+2&&now_us-start>=15000000&&now_us-start<16000000&&http_active==0);
    assert(strstr(last_url,"https://historical-forecast-api.open-meteo.com/v1/forecast?latitude=22.2707&longitude=113.5767&current="));
    before=http_count;assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);
    assert(http_count==before+1&&strstr(last_url,"//historical-forecast-api.open-meteo.com/"));
    stall_host="//historical-forecast-api.open-meteo.com/";before=http_count;
    assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);
    assert(http_count==before+2&&strstr(last_url,"//api.open-meteo.com/"));
    stall_host=NULL;
    stalled_request=true;before=http_count;start=now_us;
    assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL);
    assert(http_count==before+2&&now_us-start==30000000&&http_active==0);
    // Leaving Home during the first host must not start the fallback.
    cancel_on_delay=true;before=http_count;
    assert(home_weather_refresh(zhuhai.lat,zhuhai.lon)==ESP_FAIL&&http_count==before+1&&http_active==0);
    home_weather_allow_requests();stalled_request=false;
    assert(home_profile_set_text(&beijing)==ESP_OK);switch_city=true;
    assert(home_air_quality_refresh(beijing.lat,beijing.lon)==ESP_OK);assert(!home_air_quality_get(&air));
    assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);home_weather_init();assert(!home_air_quality_get(&air));
    before=http_count;assert(home_air_quality_refresh("22&bad=1","113.6")==ESP_ERR_INVALID_ARG);assert(http_count==before);
    // Optional fixture body is fetched separately from the actual public API.
    if(argc==2){
        char live[1024];FILE *f=fopen(argv[1],"rb");assert(f);size_t n=fread(live,1,sizeof(live)-1,f);assert(feof(f));fclose(f);live[n]=0;
        air_response=live;assert(home_air_quality_refresh(zhuhai.lat,zhuhai.lon)==ESP_OK);assert(home_air_quality_get(&air));
        printf("Live PM2.5 response parsed by production code: %u.%u ug/m3\n",(unsigned)(air.pm25_x10/10),(unsigned)(air.pm25_x10%10));
    }
    assert(http_active==0);
    puts("Weather: fetch time persisted, age text, city UTC offset (incl. +5:30) with junk rejected: PASS");
    puts("Home: coordinate validation, shared metadata, NVS failure/backward compatibility and coordinate-bound weather cache: PASS");
    puts("PM2.5: independent request, units/range/null validation, partial/oversized HTTP, city changes, offline/failure/restart, formatting and TLS release before JSON: PASS");
}
