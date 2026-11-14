#include "apps/xiaozhi/xiaozhi_protocol.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#include "apps/xiaozhi/xiaozhi_store.h"
#include "apps/reminders/reminder_service.h"
#include "device_control.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_crt_bundle.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>

static const char *TAG = "xiaozhi_proto";

namespace xiaozhi {

namespace {

// v3 二进制帧头：4 字节，payload_size 为大端。
struct __attribute__((packed)) BinaryHeaderV3 {
    uint8_t type;          // 0: audio, 1: json
    uint8_t reserved;      // 0
    uint16_t payload_size; // 网络字节序 (big-endian)
};

constexpr size_t kBinaryHeaderLen = sizeof(BinaryHeaderV3);
constexpr TickType_t kSendTimeoutMs = pdMS_TO_TICKS(150);

std::string MakeUuidV4() {
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    r[6] = (uint8_t)((r[6] & 0x0F) | 0x40);
    r[8] = (uint8_t)((r[8] & 0x3F) | 0x80);
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                  r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    return buf;
}

// Client-Id 官方要求是「持久化的随机 UUIDv4」，不是 MAC。首次生成后写入 NVS。
std::string LoadOrCreateClientId() {
    std::string stored = store_get_string(kNsXiaozhi, "client_id", "");
    if (!stored.empty()) return stored;

    std::string id = MakeUuidV4();
    if (store_set_string(kNsXiaozhi, "client_id", id)) {
        ESP_LOGI(TAG, "Generated new Client-Id UUID: %s", id.c_str());
    } else {
        ESP_LOGW(TAG, "Failed to persist Client-Id; this session uses %s", id.c_str());
    }
    return id;
}

} // namespace

Protocol::Protocol() {
    LoadIdentity();
}

void Protocol::LoadIdentity() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_buf[18];
    // 官方 Device-Id 是「小写冒号分隔」MAC（xiaozhi-esp32 system_info.cc）
    std::snprintf(mac_buf, sizeof(mac_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    mac_address_ = mac_buf;
    client_id_ = LoadOrCreateClientId();
    ESP_LOGI(TAG, "Device-Id(MAC)=%s Client-Id=%s", mac_address_.c_str(), client_id_.c_str());
}

Protocol::~Protocol() {
    // 析构只做资源释放；不允许在这里回调上层（对象正在销毁）。
    if (client_) {
        esp_websocket_client_stop(client_);
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
    if (mcp_queue_) vQueueDelete(mcp_queue_);
    if (rx_text_) heap_caps_free(rx_text_);
}

const char *Protocol::StateName(State s) {
    switch (s) {
    case State::Disconnected: return "Disconnected";
    case State::Connecting:   return "Connecting";
    case State::Idle:         return "Idle";
    case State::Listening:    return "Listening";
    case State::Thinking:     return "Thinking";
    case State::Speaking:     return "Speaking";
    case State::Error:        return "Error";
    }
    return "?";
}

bool Protocol::Start(const std::string &url, const std::string &token, int version) {
    if (client_ != nullptr || mcp_queue_ || rx_text_) {
        Stop();
    }

    if (mac_address_.empty() || client_id_.empty()) LoadIdentity();

    url_ = url;
    version_ = (version >= 1 && version <= 3) ? version : 3;
    session_id_.clear();
    hello_ok_ = false;
    critical_events_ = 0;
    needs_restart_ = false;
    ResetRx();

    rx_text_=static_cast<char*>(heap_caps_malloc(kMaxTextMessage,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if (!rx_text_) { SetState(State::Error); return false; }
    rx_text_[0]='\0';
    if (!mcp_queue_) mcp_queue_ = xQueueCreate(3, sizeof(McpRequest));
    if (!mcp_queue_) { SetState(State::Error); return false; }
    SetState(State::Connecting);

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = url_.c_str();
    // 1024 就够：小智的 JSON 报文都很小，超长报文由本层的显式分片重组兜住
    // （payload_offset/payload_len）。每 1KB 缓冲都是从 ~90KB 堆里省出来的。
    ws_cfg.buffer_size = 1024;
    ws_cfg.task_stack = 5120;          // small JSON + bounded audio enqueue; no codec work
    ws_cfg.task_prio = 5;
    ws_cfg.ping_interval_sec = 15;
    ws_cfg.disable_auto_reconnect = true; // controller owns the 6..60s backoff
    ws_cfg.network_timeout_ms = 8000;
    ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;

    // 官方握手请求头（xiaozhi-esp32 websocket_protocol.cc:97-106）
    std::string headers;
    if (!token.empty()) {
        if (token.find(' ') == std::string::npos) {
            headers += "Authorization: Bearer " + token + "\r\n";
        } else {
            headers += "Authorization: " + token + "\r\n";
        }
    }
    headers += "Protocol-Version: " + std::to_string(version_) + "\r\n";
    headers += "Device-Id: " + mac_address_ + "\r\n";
    headers += "Client-Id: " + client_id_ + "\r\n";
    ws_cfg.headers = headers.c_str();

    ESP_LOGI(TAG, "Connecting: %s (protocol v%d)", url_.c_str(), version_);
    client_ = esp_websocket_client_init(&ws_cfg);
    if (!client_) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        SetState(State::Error);
        return false;
    }

    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, WebsocketEventHandler, this);
    esp_err_t err = esp_websocket_client_start(client_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        SetState(State::Error);
        return false;
    }

    return true;
}

void Protocol::Stop() {
    device_control::cancel_session();
    // 注意：绝不能从 websocket 任务/回调里调用 —— 组件会直接拒绝
    // （"Client cannot be stopped from websocket task"）并返回失败。
    // Suppress terminal callbacks from our own stop; callbacks never destroy
    // the client. Join its worker before clearing the old session's mailbox.
    needs_restart_ = true;
    if (client_) {
        // The worker only notices stop between 1 s read polls. A PING makes the
        // server's PONG wake that poll, saving most of a second on app switch.
        if (esp_websocket_client_is_connected(client_))
            esp_websocket_client_send_with_opcode(client_, WS_TRANSPORT_OPCODES_PING, nullptr, 0,
                                                  pdMS_TO_TICKS(100));
        esp_websocket_client_stop(client_);      // 内部等待接收任务退出
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
    }
    hello_ok_ = false;
    if (mcp_queue_) { vQueueDelete(mcp_queue_); mcp_queue_ = nullptr; }
    if (rx_text_) {heap_caps_free(rx_text_);rx_text_=nullptr;}
    for (auto &slot : rejected_mcp_ids_) slot = -1;
    mcp_reply_cursor_=0;
    for (auto &reply:mcp_replies_) {reply.id=-1; std::string{}.swap(reply.response);}
    std::string{}.swap(session_id_);
    std::string{}.swap(url_);
    // Identity remains persisted in NVS. These first-session string buffers
    // otherwise survive the app and pin holes needed by the next audio codec.
    std::string{}.swap(mac_address_);
    std::string{}.swap(client_id_);
    ResetRx();
    SetState(State::Disconnected);
    critical_events_ = 0;
}

bool Protocol::IsConnected() const {
    return client_ && esp_websocket_client_is_connected(client_);
}

void Protocol::Publish(EvType type, const char *text, State st) {
    if (type == EvType::StateChanged) return;  // controller polls latest state
    if (type == EvType::Connected || type == EvType::Disconnected ||
        type == EvType::SocketError || type == EvType::HelloOk) {
        critical_events_.fetch_or(1u << static_cast<unsigned>(type));
        return;
    }
    if (event_q_ == nullptr) return;
    Event ev = {};
    ev.type = type;
    ev.state = st;
    if (text != nullptr) {
        // 安全截断：保证 NUL 结尾，且不产生半个 UTF-8 也能接受的展示效果
        std::strncpy(ev.text, text, kEventTextCap - 1);
        ev.text[kEventTextCap - 1] = '\0';
    }
    if (xQueueSend(event_q_, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropped event type=%d", (int)type);
    }
}

bool Protocol::PopCriticalEvent(Event *out) {
    if (out == nullptr) return false;
    // Teardown old resources before accepting a new hello if both arrive while
    // the controller is busy. A stale hello is checked against HandshakeDone.
    const EvType order[] = {EvType::Disconnected, EvType::SocketError,
                            EvType::Connected, EvType::HelloOk};
    for (EvType type : order) {
        const uint32_t bit = 1u << static_cast<unsigned>(type);
        if (critical_events_.fetch_and(~bit) & bit) {
            *out = {};
            out->type = type;
            return true;
        }
    }
    return false;
}

void Protocol::SetState(State new_state) {
    if (state_ == new_state) return;
    State old = state_.exchange(new_state);
    ESP_LOGI(TAG, "State: %s -> %s", StateName(old), StateName(new_state));
    Publish(EvType::StateChanged, nullptr, new_state);
}

bool Protocol::SendJson(const std::string &json) {
    if (!IsConnected()) return false;
    int ret = esp_websocket_client_send_text(client_, json.c_str(), (int)json.size(),
                                             kSendTimeoutMs);
    if (ret != static_cast<int>(json.size())) {
        ESP_LOGW(TAG, "send_text failed (%d bytes)", (int)json.size());
        return false;
    }
    return true;
}

bool Protocol::TriggerStartListen() {
    if (!IsConnected() || !HandshakeDone() || session_id_.empty()) return false;
    // 官方格式：session_id 必须回传，否则服务端无法把这一轮挂到会话上。
    std::string msg = "{\"session_id\":\"" + session_id_ +
                      "\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}";
    if (!SendJson(msg)) return false;
    ESP_LOGI(TAG, "listen/start sent");
    SetState(State::Listening);
    return true;
}

bool Protocol::TriggerStopListen() {
    if (!IsConnected()) return false;
    std::string msg = "{\"session_id\":\"" + session_id_ +
                      "\",\"type\":\"listen\",\"state\":\"stop\"}";
    if (!SendJson(msg)) return false;
    ESP_LOGI(TAG, "listen/stop sent");
    SetState(State::Thinking);
    return true;
}

bool Protocol::TriggerAbort() {
    if (!IsConnected()) return false;
    std::string msg = "{\"session_id\":\"" + session_id_ +
                      "\",\"type\":\"abort\"}";
    if (!SendJson(msg)) return false;
    ESP_LOGI(TAG, "abort sent");
    SetState(State::Idle);
    return true;
}

bool Protocol::SendAudio(const uint8_t *opus_data, size_t len) {
    if (len == 0 || opus_data == nullptr) return false;
    if (!IsConnected()) return false;

    if (version_ >= 3) {
        uint8_t send_buf[kBinaryHeaderLen + 512];
        if (kBinaryHeaderLen + len > sizeof(send_buf)) {
            ESP_LOGW(TAG, "Opus frame too large: %u", (unsigned)len);
            return false;
        }
        auto *hdr = reinterpret_cast<BinaryHeaderV3 *>(send_buf);
        hdr->type = 0;
        hdr->reserved = 0;
        hdr->payload_size = htons(static_cast<uint16_t>(len));
        std::memcpy(send_buf + kBinaryHeaderLen, opus_data, len);

        int total = (int)(kBinaryHeaderLen + len);
        int sent = esp_websocket_client_send_bin(client_, reinterpret_cast<const char *>(send_buf),
                                                total, kSendTimeoutMs);
        return sent == total;
    }

    // v1：裸 Opus，无帧头
    int sent = esp_websocket_client_send_bin(client_, reinterpret_cast<const char *>(opus_data),
                                            (int)len, kSendTimeoutMs);
    return sent == (int)len;
}

void Protocol::ResetRx() {
    rx_text_len_ = 0;
    rx_text_expect_ = 0;
    rx_opcode_ = 0;
    bin_len_ = 0;
    bin_opcode_ = 0;
}

void Protocol::WebsocketEventHandler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data) {
    (void)base;
    auto *self = static_cast<Protocol *>(handler_args);
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        self->OnConnected();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        self->OnDisconnected();
        break;
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH:
        self->OnSessionEnded();
        break;
    case WEBSOCKET_EVENT_DATA:
        self->HandleData(data);
        break;
    case WEBSOCKET_EVENT_ERROR: {
        ESP_LOGE(TAG, "WebSocket error, type=%d", data ? (int)data->error_handle.error_type : -1);
        self->hello_ok_ = false;
        self->SetState(State::Error);
        self->Publish(EvType::SocketError, "网络连接异常");
        break;
    }
    default:
        break;
    }
}

void Protocol::OnConnected() {
    if (NeedsRestart()) return;
    ESP_LOGI(TAG, "WebSocket connected");
    hello_ok_ = false;
    ResetRx();
    Publish(EvType::Connected, nullptr);

    // 官方 hello 报文：version / features{mcp} / transport / audio_params
    // Tool requests are queued by the websocket callback and executed by the
    // controller; flash writes must not block reception of audio packets.
    // frame_duration 如实上报本机编码帧长（60ms，见 xiaozhi_audio.h 的说明）。
    char hello[256];
    std::snprintf(hello, sizeof(hello),
                  "{\"type\":\"hello\",\"version\":%d,\"features\":{\"mcp\":true},"
                  "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
                  "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":%d}}",
                  version_, kEncFrameDurationMs);
    if (!SendJson(hello)) {
        ESP_LOGE(TAG, "Failed to send hello");
        Publish(EvType::SocketError, "握手报文发送失败");
    }
}

void Protocol::OnDisconnected() {
    ESP_LOGW(TAG, "WebSocket disconnected");
    hello_ok_ = false;
    SetState(State::Disconnected);
    Publish(EvType::Disconnected, nullptr);
}

void Protocol::OnSessionEnded() {
    hello_ok_ = false;
    SetState(State::Disconnected);
    // goodbye, CLOSE and FINISH can all describe the same shutdown. Keep one
    // sticky notification even when the display-text queue is full.
    if (!needs_restart_.exchange(true)) {
        Publish(EvType::Disconnected, nullptr);
    }
}

void Protocol::HandleData(const esp_websocket_event_data_t *d) {
    if (NeedsRestart()) return; // ignore late hello/TTS from a retired session
    if (d == nullptr || d->data_ptr == nullptr || d->data_len <= 0) return;

    const size_t off = (size_t)d->payload_offset;
    const size_t total = (size_t)d->payload_len;
    const size_t n = (size_t)d->data_len;

    if (d->op_code == 0x02 || (d->op_code == 0x00 && bin_len_ > 0)) {
        // 二进制：云端 Opus 音频。分片也要重组，否则半帧会被当整帧解码成噪声。
        if (d->op_code == 0x02 && off == 0) bin_len_ = 0;
        size_t space = kMaxBinaryMessage - bin_len_;
        size_t copy = (n > space) ? space : n;
        if (copy > 0) {
            std::memcpy(rx_bin_ + bin_len_, d->data_ptr, copy);
            bin_len_ += copy;
        }
        bool complete = (total == 0) || (off + n >= total);
        if (!complete && bin_len_ >= kMaxBinaryMessage) {
            ESP_LOGW(TAG, "Binary frame too large, dropped");
            bin_len_ = 0;
            return;
        }
        if (complete) {
            const uint8_t *payload = rx_bin_;
            size_t plen = bin_len_;
            if (version_ >= 3 && plen > kBinaryHeaderLen) {
                auto *hdr = reinterpret_cast<const BinaryHeaderV3 *>(rx_bin_);
                uint16_t declared = ntohs(hdr->payload_size);
                // 声明长度与实际吻合才按 v3 拆头，否则整帧交给解码器兜底
                if (hdr->type == 0 && declared == plen - kBinaryHeaderLen) {
                    payload = rx_bin_ + kBinaryHeaderLen;
                    plen = declared;
                } else {
                    ESP_LOGW(TAG, "Bad v3 header (type=%u declared=%u actual=%u), raw fallback",
                             (unsigned)hdr->type, (unsigned)declared, (unsigned)plen);
                }
            }
            // Old TTS packets after abort/stop must not restart playback. The
            // authoritative state is updated here before the controller polls.
            if (HandshakeDone() && GetState() == State::Speaking) {
                AudioService::GetInstance().PushIncomingAudio(payload, plen);
            }
            bin_len_ = 0;
        }
        return;
    }

    if (d->op_code == 0x01 || (d->op_code == 0x00 && rx_text_len_ > 0)) {
        if (!rx_text_) return;
        if (d->op_code == 0x01 && off == 0) {
            rx_text_len_ = 0;
            rx_text_expect_ = total;
        }
        if (total >= kMaxTextMessage || rx_text_len_ + n >= kMaxTextMessage) {
            // The id may sit after the cut-off params; a guessed id could answer
            // the wrong request, so the server's own timeout handles this one.
            if (off == 0) ESP_LOGW(TAG, "Text message of %u bytes exceeds %u, dropped", (unsigned)total, (unsigned)kMaxTextMessage);
            rx_text_len_ = 0;
            return;
        }
        size_t space = kMaxTextMessage - 1 - rx_text_len_;
        size_t copy = (n > space) ? space : n;
        if (copy > 0) {
            std::memcpy(rx_text_ + rx_text_len_, d->data_ptr, copy);
            rx_text_len_ += copy;
        }
        bool complete = (total == 0) || (off + n >= total);
        if (!complete && rx_text_len_ >= kMaxTextMessage - 1) {
            ESP_LOGW(TAG, "Text message exceeds %u bytes, dropped", (unsigned)kMaxTextMessage);
            rx_text_len_ = 0;
            return;
        }
        if (complete) {
            rx_text_[rx_text_len_] = '\0';
            DispatchText();
            rx_text_len_ = 0;
        }
    }
}

void Protocol::DispatchText() {
    cJSON *root = cJSON_ParseWithLength(rx_text_, rx_text_len_);
    if (root == nullptr) {
        ESP_LOGW(TAG, "Invalid JSON (%u bytes): %.80s", (unsigned)rx_text_len_, rx_text_);
        return;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type) || type->valuestring == nullptr) {
        ESP_LOGW(TAG, "Message without type: %.80s", rx_text_);
        cJSON_Delete(root);
        return;
    }

    const char *t = type->valuestring;

    if (std::strcmp(t, "hello") == 0) {
        const cJSON *sid = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(sid) && sid->valuestring) {
            session_id_ = sid->valuestring;
        }
        const cJSON *ap = cJSON_GetObjectItem(root, "audio_params");
        if (cJSON_IsObject(ap)) {
            const cJSON *sr = cJSON_GetObjectItem(ap, "sample_rate");
            const cJSON *fd = cJSON_GetObjectItem(ap, "frame_duration");
            if (cJSON_IsNumber(sr) && sr->valueint >= 8000 && sr->valueint <= 48000) {
                server_sample_rate_ = sr->valueint;
            }
            ESP_LOGI(TAG, "Server audio_params: sample_rate=%d frame_duration=%d (using %d for decode)",
                     cJSON_IsNumber(sr) ? sr->valueint : -1,
                     cJSON_IsNumber(fd) ? fd->valueint : -1, server_sample_rate_);
        }
        const cJSON *transport = cJSON_GetObjectItem(root, "transport");
        if (cJSON_IsString(transport) && transport->valuestring &&
            std::strcmp(transport->valuestring, "websocket") != 0) {
            ESP_LOGW(TAG, "Unexpected transport: %s", transport->valuestring);
        }
        ESP_LOGI(TAG, "Server hello OK, session_id='%s'", session_id_.c_str());
        hello_ok_ = true;
        Publish(EvType::HelloOk, nullptr);
        SetState(State::Idle);
    } else if (std::strcmp(t, "tts") == 0) {
        const cJSON *st = cJSON_GetObjectItem(root, "state");
        if (!cJSON_IsString(st) || st->valuestring == nullptr) {
            ESP_LOGW(TAG, "tts without state");
        } else if (std::strcmp(st->valuestring, "start") == 0) {
            SetState(State::Speaking);
        } else if (std::strcmp(st->valuestring, "stop") == 0) {
            SetState(State::Idle);
        } else if (std::strcmp(st->valuestring, "sentence_start") == 0) {
            // 官方把回复正文放在这里（llm 报文只有 emotion）
            const cJSON *txt = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(txt) && txt->valuestring) {
                Publish(EvType::AiSentence, txt->valuestring);
            }
        } else {
            ESP_LOGI(TAG, "tts state=%s (ignored)", st->valuestring);
        }
    } else if (std::strcmp(t, "stt") == 0) {
        const cJSON *txt = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(txt) && txt->valuestring) {
            Publish(EvType::UserText, txt->valuestring);
        }
        // auto 模式由服务端 VAD 决定本轮说话结束。收到识别结果就进入思考态，
        // 上层据此彻底停止录音，再允许后续 TTS 播放，保持严格半双工。
        SetState(State::Thinking);
    } else if (std::strcmp(t, "llm") == 0) {
        const cJSON *emo = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emo) && emo->valuestring) {
            Publish(EvType::LlmEmotion, emo->valuestring);
        }
        // 兜底：部分服务端仍会带 text
        const cJSON *txt = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(txt) && txt->valuestring && txt->valuestring[0]) {
            Publish(EvType::AiSentence, txt->valuestring);
        }
    } else if (std::strcmp(t, "mcp") == 0) {
        const cJSON *p = cJSON_GetObjectItem(root, "payload");
        const cJSON *id = cJSON_GetObjectItem(p, "id");
        const cJSON *method = cJSON_GetObjectItem(p, "method");
        const bool valid_id = cJSON_IsNumber(id) && id->valuedouble >= 0 && id->valuedouble <= INT32_MAX &&
            id->valuedouble == id->valueint;
        if (mcp_queue_ && valid_id && !(cJSON_IsString(method) && std::strlen(method->valuestring) < 24)) {
            RejectMcp(id->valueint);  // an answerable request with an unusable method
        } else if (mcp_queue_ && valid_id) {
            McpRequest request{}; request.id = id->valueint;
            std::strcpy(request.method, method->valuestring);
            const cJSON *params = cJSON_GetObjectItem(p, "params");
            // initialize capabilities are not needed for text reminders.
            char *printed = params && std::strcmp(request.method,"initialize") ? cJSON_PrintUnformatted(params) : nullptr;
            const char *value = printed ? printed : "{}";
            if (std::strlen(value) < sizeof(request.params)) {
                std::strcpy(request.params,value);
                if (xQueueSend(mcp_queue_,&request,0) != pdTRUE) RejectMcp(request.id);
            } else RejectMcp(request.id);
            cJSON_free(printed);
        }
    } else if (std::strcmp(t, "iot") == 0) {
        ESP_LOGD(TAG, "iot message ignored");
    } else if (std::strcmp(t, "alert") == 0 || std::strcmp(t, "error") == 0) {
        const cJSON *msg = cJSON_GetObjectItem(root, "message");
        const cJSON *status = cJSON_GetObjectItem(root, "status");
        char buf[kEventTextCap];
        std::snprintf(buf, sizeof(buf), "%s%s%s",
                      (cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring : "服务端告警",
                      (cJSON_IsString(status) && status->valuestring) ? " " : "",
                      (cJSON_IsString(status) && status->valuestring) ? status->valuestring : "");
        Publish(EvType::Alert, buf);
    } else if (std::strcmp(t, "goodbye") == 0) {
        ESP_LOGW(TAG, "Server said goodbye");
        OnSessionEnded();
    } else {
        ESP_LOGI(TAG, "Unhandled message type '%s'", t);
    }

    cJSON_Delete(root);
}

void Protocol::RejectMcp(int32_t id) {
    for (auto &slot : rejected_mcp_ids_) {
        int32_t empty = -1;
        if (slot.compare_exchange_strong(empty, id)) return;
    }
    ESP_LOGW(TAG, "MCP id=%ld refused while the rejection backlog is full", (long)id);
}

bool Protocol::ProcessMcp() {
    if (!mcp_queue_ || !HandshakeDone() || NeedsRestart()) return false;
    int32_t rejected=-1;
    for (auto &slot : rejected_mcp_ids_) if ((rejected=slot.exchange(-1))>=0) break;
    if (rejected>=0) {
        SendJson("{\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":"+
            std::to_string(rejected)+",\"error\":{\"code\":-32602,\"message\":\"Device queue or argument limit exceeded; not executed\"}}}");
        return true;
    }
    McpRequest request{};
    if (xQueueReceive(mcp_queue_,&request,0)!=pdTRUE) return false;
    uint32_t hash=2166136261u;
    for(const char *s : {request.method,request.params}) for(;*s;++s) hash=(hash^static_cast<unsigned char>(*s))*16777619u;
    const uint64_t token=((static_cast<uint64_t>(request.id)+1)<<32)|hash;
    for (const auto &reply:mcp_replies_) {
        if (reply.id==request.id && reply.hash==hash && !reply.response.empty()) {
            const bool sent=SendJson(reply.response);
            device_control::response_sent(token,sent);return true;
        }
    }
    bool error=false;
    const std::string result=reminders::mcp_result(request.method,request.params,&error,token);
    std::string response="{\"session_id\":\""+session_id_+"\",\"type\":\"mcp\",\"payload\":{\"jsonrpc\":\"2.0\",\"id\":"+
        std::to_string(request.id)+(error?",\"error\":":",\"result\":")+result+"}}";
    // Cache four recent calls even after send failure. Interleaved read/status
    // requests must not immediately make a repeated relative adjustment run twice.
    // Discovery pages are pure and do not consume this bounded call cache.
    if (!std::strcmp(request.method,"tools/call")) {
        auto &reply=mcp_replies_[mcp_reply_cursor_++ % 4];
        reply.id=request.id;reply.hash=hash;reply.response=response;
    }
    const bool sent=SendJson(response);
    device_control::response_sent(token,sent);
    ESP_LOGI(TAG,"MCP %s response_sent=%d",request.method,sent);
    return true;
}

} // namespace xiaozhi
