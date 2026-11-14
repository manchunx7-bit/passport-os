#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <ctime>
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NVS_NOT_FOUND=2, NVS_READWRITE=0;
inline const char *esp_err_to_name(int){return "injected failure";}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
using nvs_handle_t=int;
inline bool fail_write=false;
inline std::map<std::string,std::vector<unsigned char>> disk;
inline int nvs_open(const char*,int,int*h){*h=1;return ESP_OK;}
inline void nvs_close(int){}
inline int nvs_commit(int){return fail_write?ESP_FAIL:ESP_OK;}
inline int nvs_set_blob(int,const char*k,const void*v,size_t n){
    if(fail_write)return ESP_FAIL;
    auto p=static_cast<const unsigned char*>(v);disk[k]={p,p+n};return ESP_OK;
}
inline int nvs_get_blob(int,const char*k,void*v,size_t*n){
    auto i=disk.find(k);if(i==disk.end())return ESP_ERR_NVS_NOT_FOUND;
    if(i->second.size()>*n)return ESP_FAIL;
    *n=i->second.size();memcpy(v,i->second.data(),*n);return ESP_OK;
}
inline int nvs_set_u32(int h,const char*k,uint32_t v){return nvs_set_blob(h,k,&v,sizeof(v));}
inline int nvs_get_u32(int h,const char*k,uint32_t*v){size_t n=sizeof(*v);return nvs_get_blob(h,k,v,&n);}
inline int nvs_erase_key(int,const char*k){if(fail_write)return ESP_FAIL;disk.erase(k);return ESP_OK;}
using StaticSemaphore_t=int;using SemaphoreHandle_t=int*;
inline int *xSemaphoreCreateMutexStatic(int*s){return s;}
inline void xSemaphoreTake(int*,int){}
inline void xSemaphoreGive(int*){}
constexpr int portMAX_DELAY=-1;
#define pdMS_TO_TICKS(x) (x)
inline int64_t monotonic_us=1000000;
inline int64_t esp_timer_get_time(){return monotonic_us;}
inline void (*mock_delay_hook)()=nullptr;
inline void vTaskDelay(int ms){monotonic_us+=ms*1000;if(mock_delay_hook)mock_delay_hook();}
inline int bsp_audio_init_feedback(){return ESP_OK;}
inline void bsp_audio_set_volume(int){}
inline void bsp_audio_set_alert_volume(int){}
inline int bsp_audio_set_format(int,int,int){return ESP_OK;}
inline int master_volume=80;
inline int bsp_audio_get_master_volume(){return master_volume;}
inline int bsp_audio_write(const void*,size_t){return ESP_OK;}
inline void bsp_audio_deinit(){}
constexpr int UI_SFX_RATE=16000,UI_SFX_ALARM=1,UI_SFX_ALARM_VOLUME=90;
inline unsigned ui_sfx_samples(int){return 128;}
inline int16_t ui_sfx_sample(int,unsigned){return 0;}
inline void ui_sfx_cancel_alarm(){}
inline unsigned rings=0;
inline bool ui_sfx_alarm_tick(){++rings;return true;}
