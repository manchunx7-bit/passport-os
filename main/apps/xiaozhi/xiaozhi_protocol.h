#pragma once

// 小智 WebSocket 协议 v3 实现（对齐官方 xiaozhi-esp32 main/protocols/websocket_protocol.cc）
//
// 线程模型（重要）：
//   * 本类所有回调都在 esp_websocket_client 的任务上下文里同步触发。
//   * 因此回调实现必须"只入队、不做阻塞工作"，且绝不能再调用 Stop()。
//   * 出站发送（SendAudio / SendJson）允许从录音任务调用；esp_websocket_client
//     内部是可重入锁，超时是有限的（见 kSendTimeoutMs），不会死锁。
//
// 与官方实现的差异（刻意的健壮性增强）：
//   * 分片重组：官方把重组完全交给 websocket 组件；这里显式按
//     payload_offset/payload_len 重组，op_code=0x00 续帧也参与，避免 buffer_size
//     小于报文时 JSON 被逐片截断解析。
//   * 未知/告警报文会回调出来，而不是静默丢弃 —— 设备屏幕能告诉用户到底怎么了。

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <atomic>
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace xiaozhi {

enum class State {
    Disconnected,
    Connecting,
    Idle,           // 已连上小智后台，等待用户触发对话
    Listening,      // 用户正在对讲，录音并实时上传 Opus
    Thinking,       // 用户停止录音，云端 STT & LLM 思考中
    Speaking,       // 云端 TTS 正在流式下发并播放
    Error,
};

// 由 websocket 任务投递给控制任务的事件。
enum class EvType : uint8_t {
    Connected,
    Disconnected,
    SocketError,
    HelloOk,        // 服务端 hello 已收到，会话可用
    SessionInfo,    // session_id 变化
    StateChanged,   // 协议内部状态变化（Idle/Listening/Thinking/Speaking…）
    UserText,       // stt（替换语义）
    AiSentence,     // tts/sentence_start（替换语义）
    LlmEmotion,     // llm.emotion
    Alert,          // 服务端告警 / 未知报文 / 激活提示
    HandshakeTimeout,
};

constexpr size_t kEventTextCap = 256;
// Text reassembly is session-owned so its 2 KB is returned when Xiaozhi exits.
// Binary packets remain bounded; normal Opus frames are about 300 bytes.
constexpr size_t kMaxTextMessage = 2048;  // MCP initialization + bounded tool arguments
constexpr size_t kMaxBinaryMessage = 512;  // 单帧二进制（Opus）重组上限

struct Event {
    EvType type;
    State state;                 // StateChanged 时有效
    char   text[kEventTextCap];  // NUL 结尾，超长已安全截断
};

using EventQueue = QueueHandle_t;

class Protocol {
public:
    Protocol();
    ~Protocol();

    // url / token / version 由上层（NVS 或 OTA）给定。
    bool Start(const std::string &url, const std::string &token, int version);

    // 必须在控制任务里调用；不得在回调里调用（组件会直接拒绝并返回失败）。
    void Stop();

    bool IsConnected() const;
    // websocket 客户端对象当前是否存在。注意它可能"存在但未连通"：
    // esp_websocket_client 内部会按 reconnect_timeout_ms 自行重连，
    // 因此上层在这段时间里不应该反复重建客户端。
    bool HasClient() const { return client_ != nullptr; }
    // A clean close/finished worker cannot auto-reconnect even with a live handle.
    bool NeedsRestart() const { return needs_restart_.load(); }
    State GetState() const { return state_.load(); }
    const std::string &session_id() const { return session_id_; }

    void SetEventQueue(EventQueue q) { event_q_ = q; }
    void SetState(State new_state);
    // Connection/state transitions must not compete with display text for the
    // bounded queue. Connection events use a sticky mailbox; state is polled.
    bool PopCriticalEvent(Event *out);

    // 交互操作（控制任务调用）
    bool TriggerStartListen();
    bool TriggerStopListen();
    bool TriggerAbort();
    // Process at most one queued tool request on the control task. NVS work
    // must never run inside a WebSocket callback.
    bool ProcessMcp();

    // 发送压缩后的 Opus 音频帧（录音任务调用，内部有界超时）
    bool SendAudio(const uint8_t *opus_data, size_t len);

    // 握手是否在超时前完成
    bool HandshakeDone() const { return hello_ok_.load(); }

    // 服务端 hello 里 audio_params.sample_rate 的值（官方实测 24000）。
    // 播放/解码必须按它来，否则 TTS 音调和语速都会错。
    int ServerSampleRate() const { return server_sample_rate_; }

    static const char *StateName(State s);

private:
    static void WebsocketEventHandler(void *handler_args, esp_event_base_t base,
                                      int32_t event_id, void *event_data);

    void OnConnected();
    void OnDisconnected();
    void OnSessionEnded();
    void HandleData(const esp_websocket_event_data_t *data);
    void DispatchText();                                  // rx_text_ 已收全
    void Publish(EvType type, const char *text, State st = State::Disconnected);
    bool SendJson(const std::string &json);
    void LoadIdentity();
    void ResetRx();
    struct McpRequest { int32_t id; char method[24]; char params[512]; };
    QueueHandle_t mcp_queue_{nullptr};
    // Requests refused before queueing still need an error reply. A burst
    // against a full queue must not overwrite an earlier refused id.
    static constexpr size_t kRejectedSlots = 4;
    std::atomic<int32_t> rejected_mcp_ids_[kRejectedSlots]{{-1}, {-1}, {-1}, {-1}};
    void RejectMcp(int32_t id);
    struct McpReply { int32_t id=-1; uint32_t hash=0; std::string response; };
    McpReply mcp_replies_[4];
    unsigned mcp_reply_cursor_=0;

    esp_websocket_client_handle_t client_{nullptr};
    std::atomic<State> state_{State::Disconnected};
    std::atomic<uint32_t> critical_events_{0};
    EventQueue event_q_{nullptr};

    std::string mac_address_;
    std::string client_id_;
    std::string session_id_;
    std::string url_;
    int version_{3};
    std::atomic<bool> hello_ok_{false};
    std::atomic<bool> needs_restart_{false};
    int server_sample_rate_{16000};

    // 文本报文重组缓冲（仅 websocket 任务访问）
    char  *rx_text_{nullptr}; // allocated for a session, released after socket join
    size_t rx_text_len_{0};
    size_t rx_text_expect_{0};
    uint8_t rx_opcode_{0};

    // 二进制（Opus）报文重组缓冲（仅 websocket 任务访问）
    uint8_t rx_bin_[kMaxBinaryMessage];
    size_t  bin_len_{0};
    uint8_t bin_opcode_{0};
};

} // namespace xiaozhi
