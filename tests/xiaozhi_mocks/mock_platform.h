#pragma once
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101;
inline const char *esp_err_to_name(int) { return "mock error"; }
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
using TickType_t=uint32_t;
using BaseType_t=int;
using StackType_t=uint8_t;
struct StaticTask_t {};
using TaskHandle_t=void *;
constexpr int pdTRUE=1, pdFALSE=0, pdPASS=1;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
inline int64_t mock_time=1000;
inline std::function<void()> mock_on_wait;
inline int64_t esp_timer_get_time() { return mock_time*1000; }
inline TickType_t xTaskGetTickCount() { return static_cast<TickType_t>(mock_time); }
struct MockSemaphore { int count; bool mutex; };
using SemaphoreHandle_t=MockSemaphore *;
using StaticSemaphore_t=MockSemaphore;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {*s={1,true};return s;}
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s) {return xSemaphoreCreateMutexStatic(s);}
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) {*s={0,false};return s;}
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new MockSemaphore{0,false}; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new MockSemaphore{1,true}; }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new MockSemaphore{1,true}; }
inline int xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    if (!s) return pdFALSE;
    if (s->mutex) return pdTRUE;
    if (!s->count && wait) { mock_time+=wait; if(mock_on_wait) mock_on_wait(); }
    if(s->count) { --s->count; return pdTRUE; }
    return pdFALSE;
}
inline int xSemaphoreGive(SemaphoreHandle_t s) { if(s) s->count=1; return pdTRUE; }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s,TickType_t t) {return xSemaphoreTake(s,t);}
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s) {return xSemaphoreGive(s);}
inline void vSemaphoreDelete(SemaphoreHandle_t s) {delete s;}
struct MockQueue { size_t cap, size; std::deque<std::vector<uint8_t>> entries; };
using QueueHandle_t=MockQueue *;
using StaticQueue_t=MockQueue;
inline QueueHandle_t xQueueCreateStatic(size_t cap,size_t size,uint8_t *,StaticQueue_t *q) {*q={cap,size,{}};return q;}
inline QueueHandle_t xQueueCreate(size_t cap,size_t size) {return new MockQueue{cap,size,{}};}
inline int xQueueSend(QueueHandle_t q,const void *p,TickType_t) {
    if(!q || q->entries.size()>=q->cap) return pdFALSE;
    q->entries.emplace_back(static_cast<const uint8_t *>(p),static_cast<const uint8_t *>(p)+q->size);
    return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q,void *p,TickType_t) {
    if(!q || q->entries.empty()) return pdFALSE;
    std::memcpy(p,q->entries.front().data(),q->size); q->entries.pop_front(); return pdTRUE;
}
inline void vQueueDelete(QueueHandle_t q) {delete q;}
inline void xQueueReset(QueueHandle_t q) {q->entries.clear();}
inline void vTaskDelay(TickType_t ms) {mock_time+=ms; if(mock_on_wait) mock_on_wait();}
inline bool mock_task_fail=false;
inline void (*mock_pending_task)(void *)=nullptr;
inline void *mock_pending_arg=nullptr;
inline unsigned mock_task_creations=0;
inline int xTaskCreate(void (*fn)(void *),const char *,size_t,void *arg,int,TaskHandle_t *out) {
    if(mock_task_fail) return pdFALSE;
    ++mock_task_creations; mock_pending_task=fn; mock_pending_arg=arg;
    if(out) *out=reinterpret_cast<void *>(1); return pdPASS;
}
inline TaskHandle_t xTaskCreateStatic(void (*)(void *),const char *,size_t,void *,int,
                                     StackType_t *,StaticTask_t *) {
    return mock_task_fail ? nullptr : reinterpret_cast<void *>(1);
}
inline void vTaskDelete(TaskHandle_t) {}
inline void vTaskSuspend(TaskHandle_t) {}
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) {return 2800;}
inline void mock_run_task() {
    auto fn=mock_pending_task; auto arg=mock_pending_arg;
    mock_pending_task=nullptr; mock_pending_arg=nullptr; assert(fn); fn(arg);
}
constexpr int MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;
inline std::map<void *,size_t> mock_allocations;
inline std::vector<size_t> mock_alloc_order;
inline int mock_fail_size=-1;
inline size_t mock_largest=100000;
inline void *mock_malloc(size_t size) {
    if(static_cast<int>(size)==mock_fail_size) return nullptr;
    void *p=std::malloc(size); assert(p); mock_allocations[p]=size;
    mock_alloc_order.push_back(size); return p;
}
inline void mock_free(void *p) {
    if(p) {assert(mock_allocations.erase(p)==1); std::free(p);}
}
inline void *heap_caps_malloc(size_t size,int) {return mock_malloc(size);}
inline void heap_caps_print_heap_info(int) {}
inline void heap_caps_free(void *p) {mock_free(p);}
inline size_t heap_caps_get_largest_free_block(int) {return mock_largest;}
inline size_t esp_get_free_heap_size() {return 100000;}
inline size_t heap_caps_get_free_size(int) {return 100000;}
inline std::function<void()> mock_on_write;
inline std::function<void()> mock_on_read;
inline uint8_t mock_master_volume=100;
inline int mock_format_fail=0;
inline int bsp_audio_init() {return ESP_OK;}
inline void bsp_audio_deinit() {}
inline int bsp_audio_set_format(uint32_t,int,int) {return mock_format_fail;}
inline int bsp_audio_set_volume(uint8_t) {return ESP_OK;}
inline uint8_t bsp_audio_get_master_volume() {return mock_master_volume;}
inline int bsp_audio_read(void *,size_t) {if(mock_on_read) mock_on_read();return ESP_OK;}
inline int bsp_audio_write(void *,size_t) {if(mock_on_write) mock_on_write(); return ESP_OK;}
inline void settings_set_volume(uint8_t) {}
constexpr int BSP_BTN_OK=0, BSP_BTN_UP=1, BSP_BTN_DOWN=2;
class WifiManager {public: static WifiManager &GetInstance(){static WifiManager w;return w;}
    bool IsConnected() const {return true;} };

using esp_websocket_client_handle_t=void *;
using esp_event_base_t=const char *;
struct esp_websocket_event_data_t {const char *data_ptr; int data_len,payload_offset,payload_len;
    uint8_t op_code; struct {int error_type;} error_handle;};
struct esp_websocket_client_config_t {const char *uri,*headers; int buffer_size,task_stack,
    task_prio,ping_interval_sec,reconnect_timeout_ms,network_timeout_ms; bool disable_auto_reconnect; void (*crt_bundle_attach)();};
inline void esp_crt_bundle_attach() {}
constexpr int WEBSOCKET_EVENT_ANY=0,WEBSOCKET_EVENT_CONNECTED=1,WEBSOCKET_EVENT_DISCONNECTED=2,
    WEBSOCKET_EVENT_DATA=3,WEBSOCKET_EVENT_ERROR=4,WEBSOCKET_EVENT_CLOSED=5,WEBSOCKET_EVENT_FINISH=6;
inline bool mock_ws_connected=true;
inline std::vector<std::string> mock_sent_text;
inline void *esp_websocket_client_init(esp_websocket_client_config_t *) {return reinterpret_cast<void *>(1);}
inline int esp_websocket_client_start(void *) {return ESP_OK;}
inline unsigned mock_ws_stops=0, mock_ws_destroys=0;
inline unsigned mock_ws_pings=0;
inline int mock_ws_ping_result=0;
inline TickType_t mock_ws_ping_timeout=0;
inline std::vector<std::string> mock_ws_lifecycle;
enum ws_transport_opcodes_t {WS_TRANSPORT_OPCODES_PING=0x09};
inline int esp_websocket_client_send_with_opcode(void *,ws_transport_opcodes_t opcode,
                                                const uint8_t *data,int len,TickType_t timeout) {
    assert(opcode==WS_TRANSPORT_OPCODES_PING && data==nullptr && len==0);
    ++mock_ws_pings;mock_ws_ping_timeout=timeout;mock_ws_lifecycle.emplace_back("ping");
    return mock_ws_ping_result;
}
inline int esp_websocket_client_stop(void *) {++mock_ws_stops;mock_ws_lifecycle.emplace_back("stop");return ESP_OK;}
inline int esp_websocket_client_destroy(void *) {++mock_ws_destroys;mock_ws_lifecycle.emplace_back("destroy");return ESP_OK;}
inline bool esp_websocket_client_is_connected(void *) {return mock_ws_connected;}
inline void esp_websocket_register_events(void *,int,void (*)(void *,esp_event_base_t,int32_t,void *),void *) {}
inline int esp_websocket_client_send_text(void *,const char *s,int n,TickType_t) {
    mock_sent_text.emplace_back(s,n); return n;
}
inline int esp_websocket_client_send_bin(void *,const char *,int n,TickType_t) {return n;}
constexpr int ESP_MAC_WIFI_STA=0;
inline void esp_read_mac(uint8_t *out,int) {std::memset(out,0,6);}
inline void esp_fill_random(void *out,size_t n) {std::memset(out,1,n);}
#ifdef XIAOZHI_REAL_JSON
#include "../vendor/cjson/cJSON.h"
#else
struct cJSON {char *valuestring=nullptr; int valueint=0; double valuedouble=0;};
inline cJSON *cJSON_ParseWithLength(const char *,size_t) {return nullptr;}
inline cJSON *cJSON_GetObjectItem(const cJSON *,const char *) {return nullptr;}
inline bool cJSON_IsString(const cJSON *) {return false;}
inline bool cJSON_IsObject(const cJSON *) {return false;}
inline bool cJSON_IsNumber(const cJSON *) {return false;}
inline void cJSON_Delete(cJSON *) {}
#endif

using esp_audio_err_t=int;
constexpr int ESP_AUDIO_ERR_OK=0, ESP_AUDIO_MONO=1, ESP_OPUS_ENC_FRAME_DURATION_60_MS=5,
    ESP_OPUS_ENC_APPLICATION_AUDIO=1, ESP_OPUS_DEC_FRAME_DURATION_60_MS=5;
struct esp_opus_enc_config_t {int sample_rate=16000,channel=1,bits_per_sample=16,bitrate=32000,
    frame_duration=5,application_mode=1,complexity=0; bool enable_fec=false,enable_dtx=false,enable_vbr=false;};
struct esp_opus_dec_cfg_t {uint32_t sample_rate=24000; int channel=1,frame_duration=5;};
#define ESP_OPUS_ENC_CONFIG_DEFAULT() {}
#define ESP_OPUS_DEC_CONFIG_DEFAULT() {}
struct esp_audio_enc_in_frame_t {uint8_t *buffer; uint32_t len;};
struct esp_audio_enc_out_frame_t {uint8_t *buffer; uint32_t len,encoded_bytes;};
struct esp_audio_dec_in_raw_t {uint8_t *buffer; uint32_t len;};
struct esp_audio_dec_out_frame_t {uint8_t *buffer; uint32_t len,decoded_size;};
struct esp_audio_dec_info_t {};
inline bool mock_decoder_fail=false;
inline int mock_decoder_bytes=17784;
inline int mock_encoder_bytes=25000;
inline bool mock_encoder_fail=false;
extern "C" {
struct OpusDecoder;
struct OpusEncoder;
inline int opus_encoder_get_size(int) {return mock_encoder_bytes;}
inline int opus_encoder_init(OpusEncoder *state,int32_t,int,int) {
    if(mock_encoder_fail)return -1;
    std::memset(state,0xe1,mock_encoder_bytes);return 0;
}
inline OpusEncoder *__real_opus_encoder_create(int32_t,int,int,int *error) {
    if(error)*error=0;return static_cast<OpusEncoder *>(mock_malloc(mock_encoder_bytes));
}
inline void __real_opus_encoder_destroy(OpusEncoder *p) {mock_free(p);}
OpusEncoder *__wrap_opus_encoder_create(int32_t,int,int,int *);
void __wrap_opus_encoder_destroy(OpusEncoder *);
inline int opus_decoder_get_size(int) {return mock_decoder_bytes;}
inline int opus_decoder_init(OpusDecoder *state,int32_t rate,int channels) {
    if (mock_decoder_fail || channels!=1 || (rate!=16000 && rate!=24000)) return -1;
    std::memset(state, 0, mock_decoder_bytes); return 0;
}
inline int opus_decode(OpusDecoder *,const unsigned char *,int32_t,int16_t *,int frame_size,int) {return frame_size;}
}
inline int esp_opus_enc_open(void *,size_t,void **out) {
    *out=mock_malloc(56);if(!*out)return -1;
    int error=0;auto *state=__wrap_opus_encoder_create(16000,1,2049,&error);
    if(!state){mock_free(*out);*out=nullptr;return error;}
    *static_cast<OpusEncoder **>(*out)=state;return 0;
}
inline void esp_opus_enc_close(void *p) {
    __wrap_opus_encoder_destroy(*static_cast<OpusEncoder **>(p));mock_free(p);
}
inline int esp_opus_enc_process(void *p,esp_audio_enc_in_frame_t *,esp_audio_enc_out_frame_t *out) {
    assert(*reinterpret_cast<uint8_t *>(*static_cast<OpusEncoder **>(p))==0xe1);
    out->encoded_bytes=240; return 0;
}
inline int esp_opus_dec_open(void *,size_t,void **out) {
    if(mock_decoder_fail) return -1; *out=mock_malloc(12000);return *out?0:-1;
}
inline void esp_opus_dec_close(void *p) {mock_free(p);}
inline int esp_opus_dec_decode(void *,esp_audio_dec_in_raw_t *,esp_audio_dec_out_frame_t *out,
                               esp_audio_dec_info_t *) {out->decoded_size=2880; return 0;}
