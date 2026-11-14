// main/apps/voice/udp_audio.c —— Wi-Fi UDP 音频/事件通道实现。
// 协议契约见 docs/voice-udp-channel.md(与 companion/udp_transport.py 同构)。
#include "udp_audio.h"

#include "app_protocol.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "udp_audio";

// 一个音频块固定 804B(4B 块头 + 800B 4bit 数据),见 apps/voice/adpcm.h
#define ADPCM_BLOCK_MAX 804
// 上行缓冲:[type 1B][payload];最大 payload = 2B 帧头 + 804B 块
#define TX_BUF_MAX (1 + 2 + ADPCM_BLOCK_MAX)
// 下行接收缓冲:最大 payload = 513B(EVENT),留余量以识别异常大包
#define RX_BUF_MAX 640
// 对端"消失过又回来"的判定阈值。注意:这里的 *1000 是 ms→us 换算,结果就是
// 链路超时本身(3000ms = 3e6us),并非注释旧文所写的"3 倍"。语义上正好:
// beacon 正常时每秒一发(间隔 ~1s < 3s 不触发),一旦停了 3s 以上说明链路已断,
// 下一发 beacon 就重发 device.hello 让 companion 重新学到我们。
#define PEER_STALE_US ((int64_t)VOICE_UDP_PEER_TIMEOUT_MS * 1000)
#define MY_IP_STR(buf) ((buf)[0] ? (buf) : "0.0.0.0")

static int s_sock = -1;
static volatile bool s_active = false;
static TaskHandle_t s_rx_task = NULL;
static SemaphoreHandle_t s_tx_mux = NULL;
static StaticSemaphore_t s_tx_mux_storage;

// 已学到的对端地址(只有 RX 任务在收到 beacon 时写;s_peer_mux 保护)
static struct sockaddr_in s_peer;
static volatile bool s_peer_known = false;
static volatile int64_t s_peer_last_us = 0;
static portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;

// 上行缓冲:init 时分配、deinit 时释放,仅在持有 s_tx_mux 时访问
static uint8_t *s_tx_buf = NULL;
static uint8_t s_audio_seq = 0;

static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_audio_drops = 0;
static uint32_t s_event_drops = 0;
static volatile uint32_t s_beacon_cnt = 0;   // 链路诊断:收到 beacon 总数
static volatile uint32_t s_ping_cnt = 0;     // 链路诊断:发出 ping 总数

// ---- 内部工具 ----

static void drop_inc(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stat_mux);
    (*counter)++;
    portEXIT_CRITICAL(&s_stat_mux);
}

static uint32_t drop_get(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stat_mux);
    uint32_t v = *counter;
    portEXIT_CRITICAL(&s_stat_mux);
    return v;
}

// 取对端地址快照。返回 false = 尚未学到对端(链路未 up)。
static bool peer_snapshot(struct sockaddr_in *out)
{
    bool known;
    portENTER_CRITICAL(&s_peer_mux);
    known = s_peer_known;
    if (known) *out = s_peer;
    portEXIT_CRITICAL(&s_peer_mux);
    return known;
}

// 链路是否新鲜:对端已知且最近 VOICE_UDP_PEER_TIMEOUT_MS 内收到过对端报文。
static bool peer_fresh(void)
{
    int64_t now = esp_timer_get_time();
    bool fresh = false;
    portENTER_CRITICAL(&s_peer_mux);
    fresh = s_peer_known && s_peer_last_us != 0 &&
            (now - s_peer_last_us) < (int64_t)VOICE_UDP_PEER_TIMEOUT_MS * 1000;
    portEXIT_CRITICAL(&s_peer_mux);
    return fresh;
}

// 单播一发。对端未知 / socket 未就绪 / 发送失败返回 -1。
// s_tx_buf 为共享缓冲,由 s_tx_mux 串行化(audio worker 与 app task 是两个任务)。
// head 为可选的 payload 前缀(音频帧头),直接组装进 s_tx_buf,省去额外缓冲。
static int tx_send_parts(uint8_t type, const uint8_t *head, size_t head_len,
                         const void *payload, size_t len)
{
    if (s_sock < 0 || head_len + len + 1 > TX_BUF_MAX || s_tx_mux == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "TX 互斥等待超时,丢弃该帧");
        return -1;
    }
    int rc = -1;
    struct sockaddr_in peer;
    if (s_tx_buf != NULL && peer_snapshot(&peer)) {
        s_tx_buf[0] = type;
        if (head_len > 0) memcpy(s_tx_buf + 1, head, head_len);
        if (len > 0) memcpy(s_tx_buf + 1 + head_len, payload, len);
        len += head_len;
        ssize_t n = sendto(s_sock, s_tx_buf, len + 1, 0,
                           (const struct sockaddr *)&peer, sizeof(peer));
        if (n == (ssize_t)(len + 1)) {
            rc = 0;
        } else {
            ESP_LOGW(TAG, "sendto 失败: rc=%d errno=%d", (int)n, errno);
        }
    }
    xSemaphoreGive(s_tx_mux);
    return rc;
}

static int tx_send(uint8_t type, const void *payload, size_t len)
{
    return tx_send_parts(type, NULL, 0, payload, len);
}

static void send_device_hello(void)
{
    char buf[128];
    size_t n = app_protocol_device_hello(buf, sizeof(buf), 1);
    if (n > 0) {
        udp_audio_notify_event(buf, n);
    }
}

// 判活:每 VOICE_UDP_PING_MS 给 companion 一发空载荷 PING。
// 设备空闲时不产生任何报文,companion 就分不清"设备还在"和"设备退出了语音应用"
// —— 这一发是它 6s 静默判断开的唯一依据。链路未通时不发(省得白算)。
// 不计入丢帧统计:判活失败由 beacon/下次报文自然覆盖,不是音频/事件语义的丢失。
static void send_liveness_ping(void)
{
    if (!peer_fresh()) return;
    (void)tx_send(VOICE_UDP_T_PING, NULL, 0);
    s_ping_cnt++;
}

// beacon:建立或刷新对端。返回 true = 需要回一发 device.hello。
static bool peer_accept_beacon(const struct sockaddr_in *src)
{
    int64_t now = esp_timer_get_time();
    bool need_hello = false;
    portENTER_CRITICAL(&s_peer_mux);
    bool same = s_peer_known && s_peer.sin_addr.s_addr == src->sin_addr.s_addr;
    if (!same) {
        s_peer.sin_family = AF_INET;
        s_peer.sin_addr   = src->sin_addr;
        s_peer.sin_port   = htons(VOICE_UDP_PORT);
        s_peer_known      = true;
        need_hello        = true;
    } else if (s_peer_last_us == 0 || now - s_peer_last_us > PEER_STALE_US) {
        need_hello = true;   // 对端消失过又回来:让 companion 重新学到我们
    }
    s_peer_last_us = now;
    portEXIT_CRITICAL(&s_peer_mux);
    return need_hello;
}

// 非 beacon 的下行:仅在与已建立对端同源时续命(防第三方刷报文续活链路)
static void peer_refresh(const struct sockaddr_in *src)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_peer_mux);
    if (s_peer_known && s_peer.sin_addr.s_addr == src->sin_addr.s_addr) {
        s_peer_last_us = now;
    }
    portEXIT_CRITICAL(&s_peer_mux);
}

static void rx_task(void *arg)
{
    // 接收缓冲由 init 分配、本任务退出时释放(deinit 超时后任务仍可能在用)。
    // 多 1B 用于原地补 '\0',事件 JSON 直接在 rx 内解析。
    uint8_t *rx = arg;
    int64_t last_ping_us = 0;
    ESP_LOGI(TAG, "UDP RX task started (port %d)", VOICE_UDP_PORT);

    while (s_active) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(s_sock, rx, RX_BUF_MAX, 0,
                             (struct sockaddr *)&src, &slen);
        // n <= 0 是 SO_RCVTIMEO(200ms)到期,正常路径 → 直接落到下面的判活节拍
        if (n > 0) {
            const uint8_t type = rx[0];
            if (type == VOICE_UDP_T_BEACON) {
                s_beacon_cnt++;
                if (peer_accept_beacon(&src)) {
                    ESP_LOGI(TAG, "companion beacon 来自 %s → link up",
                             inet_ntoa(src.sin_addr));
                    send_device_hello();
                }
            } else if (type == VOICE_UDP_T_EVENT) {
                struct sockaddr_in peer;
                if (!peer_snapshot(&peer) || peer.sin_addr.s_addr != src.sin_addr.s_addr ||
                    src.sin_port != peer.sin_port) continue;
                peer_refresh(&src);
                size_t plen = (size_t)n - 1;   // <= RX_BUF_MAX - 1 < APP_PROTO_RX_CAP
                char *json = (char *)rx + 1;
                json[plen] = '\0';
                app_event_t ev;
                if (app_protocol_parse(json, plen, &ev)) {
                    app_protocol_dispatch_event(&ev);   // 审批重要投递,其余零阻塞
                } else {
                    ESP_LOGW(TAG, "下行 JSON 拒绝: %.*s",
                             (int)(plen > 80 ? 80 : plen), json);
                }
            } else {
                ESP_LOGW(TAG, "未知报文类型 0x%02x(%d B),忽略", type, (int)n);
            }
        }

        // 判活节拍:与收包解耦(这一轮有没有报文都要跑)
        int64_t now_us = esp_timer_get_time();
        if (last_ping_us == 0 ||
            now_us - last_ping_us >= (int64_t)VOICE_UDP_PING_MS * 1000) {
            last_ping_us = now_us;
            send_liveness_ping();
        }
        // 链路诊断(每 5s):beacon/ping 计数不涨的一侧就是断点
        static int64_t last_diag_us = 0;
        if (last_diag_us == 0 ||
            now_us - last_diag_us >= 5000000) {
            last_diag_us = now_us;
            bool known;
            int64_t age_ms;
            portENTER_CRITICAL(&s_peer_mux);
            known = s_peer_known;
            age_ms = s_peer_last_us ? (now_us - s_peer_last_us) / 1000 : -1;
            portEXIT_CRITICAL(&s_peer_mux);
            ESP_LOGI(TAG, "link diag: beacons=%u pings=%u peer_known=%d peer_age_ms=%ld",
                     (unsigned)s_beacon_cnt, (unsigned)s_ping_cnt,
                     (int)known, (long)age_ms);
        }
    }
    ESP_LOGI(TAG, "UDP RX task exiting");
    free(rx);
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

// ---- 公开接口 ----

int udp_audio_init(void)
{
    if (s_sock >= 0) return 0;   // 幂等

    if (s_tx_mux == NULL) {
        s_tx_mux = xSemaphoreCreateMutexStatic(&s_tx_mux_storage);
        if (s_tx_mux == NULL) {
            ESP_LOGE(TAG, "TX 互斥创建失败");
            return -1;
        }
    }

    if (s_tx_buf == NULL) {
        uint8_t *tx = malloc(TX_BUF_MAX);
        if (tx == NULL) {
            ESP_LOGE(TAG, "TX 缓冲分配失败");
            return -1;
        }
        xSemaphoreTake(s_tx_mux, portMAX_DELAY);
        s_tx_buf = tx;
        xSemaphoreGive(s_tx_mux);
    }
    uint8_t *rx = malloc(RX_BUF_MAX + 1);
    if (rx == NULL) {
        ESP_LOGE(TAG, "RX 缓冲分配失败");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket 创建失败: errno=%d", errno);
        free(rx);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(VOICE_UDP_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d 失败: errno=%d", VOICE_UDP_PORT, errno);
        close(sock);
        free(rx);
        return -1;
    }
    // 接收超时:让 RX 任务能周期性复查停止位(deinit 才能干净退出)
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200 * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    // 允许发往广播地址(设备当前不发广播,保留给将来的主动探测)
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    s_sock = sock;
    s_active = true;
    if (xTaskCreate(rx_task, "udp_rx", 4096, rx, 4, &s_rx_task) != pdPASS) {
        ESP_LOGE(TAG, "RX 任务创建失败");
        s_active = false;
        free(rx);
        close(sock);
        s_sock = -1;
        return -1;
    }
    ESP_LOGI(TAG, "UDP 通道就绪(port %d),等待 companion beacon", VOICE_UDP_PORT);
    return 0;
}

int udp_audio_deinit(void)
{
    if (s_sock < 0 && s_rx_task == NULL && s_tx_buf == NULL) return 0;   // 幂等

    // 1. 先置停止位:recvfrom 的 200ms 超时会让任务回到循环顶看到它
    s_active = false;
    // 2. 等任务自己退出再关 socket —— close 与阻塞中的 recvfrom 并发是竞态
    for (int i = 0; i < 50 && s_rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_rx_task != NULL) {
        ESP_LOGW(TAG, "RX 任务未在 500ms 内退出(继续关闭,任务将自行收敛)");
    }
    // 3. 关 socket
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    // 4. 清对端状态:下次进入重新发现
    portENTER_CRITICAL(&s_peer_mux);
    s_peer_known  = false;
    s_peer_last_us = 0;
    memset(&s_peer, 0, sizeof(s_peer));
    portEXIT_CRITICAL(&s_peer_mux);
    s_audio_seq = 0;
    // 5. 释放 TX 缓冲(持锁,与仍在收尾的发送者互斥);RX 缓冲由任务自行释放
    if (s_tx_mux != NULL) {
        xSemaphoreTake(s_tx_mux, portMAX_DELAY);
        free(s_tx_buf);
        s_tx_buf = NULL;
        xSemaphoreGive(s_tx_mux);
    }
    ESP_LOGI(TAG, "UDP 通道已关闭");
    return 0;
}

int udp_audio_notify_audio(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0 || len > ADPCM_BLOCK_MAX) return -1;
    // UDP 下每块恒为单片且是末片 → 帧头 [seq][0x80],与 companion 的
    // reassemble_adpcm() 期望的"新 seq + is_last"完全一致。
    const uint8_t head[2] = {s_audio_seq++, 0x80};
    int rc = tx_send_parts(VOICE_UDP_T_AUDIO, head, sizeof(head), frame, len);
    if (rc != 0) drop_inc(&s_audio_drops);
    return rc;
}

int udp_audio_notify_event(const char *line, size_t len)
{
    if (line == NULL || len == 0) return -1;
    int rc = tx_send(VOICE_UDP_T_EVENT, line, len);
    if (rc != 0) drop_inc(&s_event_drops);
    return rc;
}

int udp_audio_notify_event_blocking(const char *line, size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;   // UDP 无发送队列与背压,无等待语义
    return udp_audio_notify_event(line, len);
}

bool udp_audio_connected(void)        { return peer_fresh(); }
bool udp_audio_event_subscribed(void) { return peer_fresh(); }

uint32_t udp_audio_audio_drops(void) { return drop_get(&s_audio_drops); }
uint32_t udp_audio_event_drops(void) { return drop_get(&s_event_drops); }

const char *udp_audio_peer_ip(void)
{
    static char buf[16];
    struct sockaddr_in peer;
    if (!peer_snapshot(&peer)) {
        return "0.0.0.0";
    }
    const char *s = inet_ntoa(peer.sin_addr);
    strncpy(buf, s ? s : "0.0.0.0", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return MY_IP_STR(buf);
}
