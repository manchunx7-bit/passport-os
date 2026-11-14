#include "apps/netease/netease_player.h"

#include "apps/netease/netease_api.h"
#include "apps/netease/netease_dsp.h"

#include "apps/radio/radio_source.h"
#include "apps/radio/radio_stream_buffer.h"
#include "audio_workspace.h"
#include "bsp_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <algorithm>
#include <atomic>

namespace netease {

namespace {

constexpr char kTag[] = "netease_player";
// Same realtime layout as Radio: [music task stack | input | PCM out | reader stack].
constexpr std::size_t kMusicStackBytes = 7168;
constexpr std::size_t kInputSize = 1024;
constexpr std::size_t kOutputSize = 8192;
constexpr std::size_t kReaderStackBytes = 6144;
static_assert(kMusicStackBytes + kInputSize + kOutputSize + kReaderStackBytes <= audio_workspace::kRealtimeBytes);
static_assert(sizeof(StaticTask_t) <= sizeof(PlayerTasks::reader_tcb));

std::atomic<uint32_t> s_elapsed_ms{0};
std::atomic<bool> s_buffering{false};
bool s_registered = false;
std::atomic<uint8_t> s_levels[kMeterBars]{};
void clear_levels() { for (auto &level : s_levels) level.store(0, std::memory_order_relaxed); }

struct Reader {
    radio_stream::Buffer buffer;
    RadioSource &source;
    std::atomic<bool> stop{false}, finished{false};
    Reader(uint8_t *storage, std::size_t capacity, RadioSource &http) : buffer(storage, capacity), source(http) {}
};

void reader_task(void *arg) {
    auto &reader = *static_cast<Reader *>(arg);
    unsigned empty_reads = 0;
    while (!reader.stop.load(std::memory_order_acquire)) {
        std::size_t room = 0;
        uint8_t *target = reader.buffer.write_data(room);
        if (!room) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        const int received = reader.source.read(target, std::min<std::size_t>(room, kInputSize));
        if (received < 0) break;
        if (!received) {
            if (reader.source.complete() || ++empty_reads > 3) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        empty_reads = 0;
        reader.buffer.commit(received);
    }
    reader.finished.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}

std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (channels <= 1) return bytes & ~static_cast<std::size_t>(1);
    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    for (std::size_t f = 0; f < frames; ++f) {
        int32_t mixed = 0;
        for (uint8_t c = 0; c < channels; ++c) mixed += samples[f * channels + c];
        samples[f] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

}  // namespace

bool player_init() {
    if (bsp_audio_init_playback() != ESP_OK) return false;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) { bsp_audio_deinit(); return false; }
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        esp_audio_dec_unregister_default();
        bsp_audio_deinit();
        return false;
    }
    s_registered = true;
    bsp_audio_set_volume(100);
    return true;
}

void player_deinit() {
    if (s_registered) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        s_registered = false;
    }
    bsp_audio_deinit();
}

void player_reset_status() {
    s_elapsed_ms.store(0, std::memory_order_relaxed);
    s_buffering.store(true, std::memory_order_relaxed);
    clear_levels();
}

uint32_t player_elapsed_ms() { return s_elapsed_ms.load(std::memory_order_relaxed); }
void player_audio_levels(uint8_t (&levels)[kMeterBars]) {
    for (size_t i = 0; i < kMeterBars; ++i) levels[i] = s_levels[i].load(std::memory_order_relaxed);
}
bool player_buffering() { return s_buffering.load(std::memory_order_relaxed); }

PlayOutcome play(const PlayRequest &req, PlayerTasks &tasks) {
    clear_levels();
    PlayOutcome out{PlayEnd::Failed, req.offset, req.total_bytes, false};
    const auto base_ms = [&](uint32_t offset, uint32_t total) -> uint32_t {
        return total && req.duration_ms ? static_cast<uint32_t>(uint64_t(req.duration_ms) * offset / total) : 0;
    };
    s_elapsed_ms.store(base_ms(req.offset, req.total_bytes), std::memory_order_relaxed);
    s_buffering.store(true, std::memory_order_relaxed);

    struct Cancel { std::atomic<bool> stop{false}; bool (*interrupted)(); } cancel;
    cancel.interrupted = req.interrupted;
    RadioSource source([](void *c) {
        auto &cancel = *static_cast<Cancel *>(c);
        return cancel.stop.load(std::memory_order_acquire) || cancel.interrupted();
    }, &cancel);
    // Logged-in CDN URLs carry a long signature: a 512 B request buffer truncated it (403).
    source.set_client(browser_agent(), "https://music.163.com/", 2048);

    if (!source.open(req.url, req.offset)) {
        const int status = source.status();
        out.end = req.interrupted() ? PlayEnd::Interrupted
                  : source.oom() ? PlayEnd::NoMemory
                  : (status == 403 || status == 404 || status == 410 || status == 416) ? PlayEnd::Expired
                  : PlayEnd::NetworkLost;
        if (out.end != PlayEnd::Interrupted)
            ESP_LOGW(kTag, "Open failed: status=%d end=%d", status, int(out.end));
        s_buffering.store(false, std::memory_order_relaxed);
        return out;
    }
    // A server that ignores Range restarts the file from byte 0.
    uint32_t offset = source.status() == 206 ? req.offset : 0;
    if (source.content_length()) out.total_bytes = offset + source.content_length();
    const uint32_t start_ms = base_ms(offset, out.total_bytes);
    out.position = offset;

    esp_audio_simple_dec_cfg_t cfg{source.type(), nullptr, 0, false};
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t opened = esp_audio_simple_dec_open(&cfg, &decoder);
    if (opened != ESP_AUDIO_ERR_OK) {
        out.end = opened == ESP_AUDIO_ERR_MEM_LACK ? PlayEnd::NoMemory : PlayEnd::Failed;
        ESP_LOGE(kTag, "Decoder open failed: %d free=%u largest=%u", opened, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        s_buffering.store(false, std::memory_order_relaxed);
        return out;
    }

    uint8_t *input = audio_workspace::storage + kMusicStackBytes;
    uint8_t *output = input + kInputSize;
    constexpr std::size_t capacity = audio_workspace::music::kCompressedBytes;
    Reader reader(audio_workspace::aux(), capacity, source);
    TaskHandle_t reader_handle = xTaskCreateStatic(reader_task, "music_http", kReaderStackBytes, &reader, 5,
        reinterpret_cast<StackType_t *>(output + kOutputSize), reinterpret_cast<StaticTask_t *>(tasks.reader_tcb));
    if (!reader_handle) {
        esp_audio_simple_dec_close(decoder);
        s_buffering.store(false, std::memory_order_relaxed);
        return out;
    }

    bool format_ready = false, buffering = true, failed = false, no_memory = false;
    uint8_t channels = 1;
    uint32_t rate = 0;
    uint64_t samples = 0;
    uint32_t fed = 0, meter_at = UINT32_MAX;
    SpeakerDsp dsp;
    while (!req.interrupted()) {
        const bool finished = reader.finished.load(std::memory_order_acquire);
        const std::size_t available = reader.buffer.available();
        if (finished && !available) break;
        // Match the existing recovery reserve; filling an extra 4 KiB before
        // the first frame only delayed startup on realtime/paced sources.
        const std::size_t resume_at = capacity / 2;
        if (buffering && available < resume_at && !finished) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (buffering) s_buffering.store(false, std::memory_order_relaxed);
        buffering = false;
        const std::size_t received = reader.buffer.read(input, kInputSize);
        if (!received) {
            buffering = true;
            clear_levels();
            s_buffering.store(true, std::memory_order_relaxed);
            continue;
        }
        fed += received;
        const bool last = reader.finished.load(std::memory_order_acquire) && source.complete() &&
                          !reader.buffer.available();
        esp_audio_simple_dec_raw_t raw{input, static_cast<uint32_t>(received), last, 0,
                                       ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE};
        while (raw.len > 0 && !req.interrupted()) {
            esp_audio_simple_dec_out_t frame{output, kOutputSize, 0, 0};
            const uint32_t before = raw.len;
            const esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (result == ESP_AUDIO_ERR_MEM_LACK) { no_memory = true; break; }
            if (result != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(kTag, "Decode error %d", result);
                failed = true;
                break;
            }
            if (raw.consumed > raw.len) raw.consumed = raw.len;
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (frame.decoded_size > 0) {
                if (!format_ready) {
                    esp_audio_simple_dec_info_t info{};
                    if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) continue;
                    channels = std::clamp<uint8_t>(info.channel, 1, 2);
                    rate = info.sample_rate;
                    if (info.bits_per_sample != 16 || rate < 8000 || rate > 48000 ||
                        bsp_audio_set_format(rate, 16, 1) != ESP_OK) {
                        ESP_LOGE(kTag, "Unsupported format %luHz/%ubit", (unsigned long)rate, info.bits_per_sample);
                        failed = true;
                        break;
                    }
                    bsp_audio_set_volume(100);
                    format_ready = true;
                }
                const std::size_t mono = downmix_to_mono(frame.buffer, frame.decoded_size, channels);
                // High-pass + limiter so loud masters do not clip at high volume.
                dsp.configure(rate, bsp_audio_get_master_volume());
                dsp.process(reinterpret_cast<int16_t *>(frame.buffer), mono / sizeof(int16_t));
                const uint32_t meter_tick = static_cast<uint32_t>(samples * 1000 / rate) / 50;
                if (meter_tick != meter_at) {
                    uint8_t levels[kMeterBars];
                    meter_levels(reinterpret_cast<int16_t *>(frame.buffer), mono / sizeof(int16_t), levels);
                    for (size_t i = 0; i < kMeterBars; ++i) s_levels[i].store(levels[i], std::memory_order_relaxed);
                    meter_at = meter_tick;
                }
                if (bsp_audio_write(frame.buffer, mono) == ESP_OK) {
                    out.played = true;
                    samples += mono / sizeof(int16_t);
                    s_elapsed_ms.store(start_ms + static_cast<uint32_t>(samples * 1000 / rate), std::memory_order_relaxed);
                }
            }
            // The parser keeps a short incomplete tail; wait for more bytes.
            if (raw.len == before || raw.consumed == 0) break;
        }
        if (failed || no_memory) break;
    }
    clear_levels();
    const bool interrupted = req.interrupted();
    const bool complete = reader.finished.load(std::memory_order_acquire) && source.complete() &&
                          !reader.buffer.available();
    // Never close HTTP or reuse the FIFO/stack while a read is in flight.
    reader.stop.store(true, std::memory_order_release);
    cancel.stop.store(true, std::memory_order_release);
    while (!reader.finished.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(10));
    vTaskDelete(reader_handle);
    esp_audio_simple_dec_close(decoder);

    out.position = offset + fed;
    out.end = no_memory ? PlayEnd::NoMemory
              : failed ? PlayEnd::Failed
              : interrupted ? PlayEnd::Interrupted
              : complete ? PlayEnd::Finished
              : PlayEnd::NetworkLost;
    s_buffering.store(false, std::memory_order_relaxed);
    return out;
}

}  // namespace netease
