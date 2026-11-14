#include "radio_presets.h"
#include "radio_player.h"
#include "radio_stream_buffer.h"
#include "radio_source.h"

#include "bsp_audio.h"
#include "audio_workspace.h"
#include "launcher/settings.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio_ui.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {

constexpr char kTag[] = "radio_player";
constexpr std::size_t kStreamStackBytes = 7168;
constexpr std::size_t kInputSize = 1024;
constexpr std::size_t kOutputSize = 8192;
constexpr std::size_t kReaderStackBytes = 6144;
static_assert(kStreamStackBytes + kInputSize + kOutputSize + kReaderStackBytes <= audio_workspace::kRealtimeBytes);
StaticTask_t s_stream_tcb;
StaticTask_t s_reader_tcb;
constexpr std::size_t kLevelCount = 18;
constexpr std::size_t kMaxStations = 10;
// Consecutive silent failures tolerated before moving to the next station.
constexpr uint8_t kMaxFailedAttempts = 3;
// Two dead stations in a row is a network problem (hotspot without data,
// captive portal), not bad stations: stop walking the list and keep retrying.
constexpr uint8_t kMaxAutoSkips = 2;

std::atomic<bool> s_network_connected{false};
std::atomic<bool> s_wanted_playing{true};
std::atomic<std::size_t> s_station_index{0};
std::atomic<std::size_t> s_station_count{0};
std::atomic<uint32_t> s_generation{1};
std::atomic<bool> s_stream_active{false};
std::atomic<bool> s_shutdown_requested{false};
std::atomic<TaskHandle_t> s_player_task{nullptr};
SemaphoreHandle_t s_player_stopped;
SemaphoreHandle_t s_station_mutex;
bool s_decoders_registered;
RadioStation *s_stations;

RadioStation station_snapshot(std::size_t index) {
    RadioStation station = {};
    if (!s_stations || !s_station_mutex ||
        xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return station;
    }
    const std::size_t count = std::max<std::size_t>(s_station_count.load(), 1);
    station = s_stations[index % count];
    xSemaphoreGive(s_station_mutex);
    return station;
}

void save_setting(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

uint8_t load_setting(const char *key, uint8_t fallback) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

void save_station_name(const char *name) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    if (name && name[0]) {
        nvs_set_str(handle, "station_name", name);
    } else {
        nvs_erase_key(handle, "station_name");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

// Copies the station name the listener last chose. Returns false when no
// station has been remembered yet.
bool load_station_name(char *name, std::size_t capacity) {
    if (!name || capacity == 0) return false;
    name[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = capacity;
    const esp_err_t error = nvs_get_str(handle, "station_name", name, &length);
    nvs_close(handle);
    if (error != ESP_OK) {
        name[0] = '\0';
        return false;
    }
    return name[0] != '\0';
}

// Installs a catalog under the station mutex and reports the index that should
// stay selected. `preferred_name` keeps the previous station when the fresh
// catalog still carries it.
bool install_stations(const RadioStation *stations, std::size_t count,
                      const char *preferred_name, std::size_t *selected) {
    if (!stations || count == 0 || !s_stations || !s_station_mutex) return false;
    count = std::min(count, kMaxStations);

    // Stop the current request before replacing its backing catalog. The
    // streaming task uses a value snapshot, so the swap is atomic to users.
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    std::memcpy(s_stations, stations, count * sizeof(RadioStation));
    if (count < kMaxStations) {
        std::memset(s_stations + count, 0,
                    (kMaxStations - count) * sizeof(RadioStation));
    }
    std::size_t index = 0;
    if (preferred_name && preferred_name[0]) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(s_stations[i].name, preferred_name) == 0) {
                index = i;
                break;
            }
        }
    }
    s_station_count.store(count, std::memory_order_release);
    s_station_index.store(index, std::memory_order_release);
    xSemaphoreGive(s_station_mutex);
    if (selected) *selected = index;
    return true;
}

bool request_still_current(uint32_t generation, std::size_t station) {
    return !s_shutdown_requested.load(std::memory_order_acquire) &&
           s_network_connected.load(std::memory_order_acquire) &&
           s_wanted_playing.load(std::memory_order_acquire) &&
           s_generation.load(std::memory_order_acquire) == generation &&
           s_station_index.load(std::memory_order_acquire) == station;
}

void calculate_levels(const uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || bytes < 64) return;
    const auto *samples = reinterpret_cast<const int16_t *>(pcm);
    const std::size_t sample_count = bytes / sizeof(int16_t);
    if (sample_count < kLevelCount) return;
    const std::size_t frames = sample_count / std::max<uint8_t>(channels, 1);
    if (frames < kLevelCount) return;

    uint8_t levels[kLevelCount] = {};
    for (std::size_t band = 0; band < kLevelCount; ++band) {
        const std::size_t begin = band * frames / kLevelCount;
        const std::size_t end = (band + 1) * frames / kLevelCount;
        int64_t energy = 0;
        std::size_t count = 0;
        for (std::size_t frame = begin; frame < end; ++frame) {
            int32_t mixed = 0;
            for (uint8_t channel = 0; channel < channels; ++channel) {
                mixed += samples[frame * channels + channel];
            }
            mixed /= std::max<uint8_t>(channels, 1);
            energy += static_cast<int64_t>(mixed) * mixed;
            ++count;
        }
        const float rms = count ? std::sqrt(static_cast<float>(energy) / count) : 0.0f;
        float db = rms > 1.0f ? 20.0f * std::log10(rms / 32768.0f) : -80.0f;
        int value = static_cast<int>((db + 58.0f) * 2.25f);
        value += static_cast<int>((band * 13 + s_generation.load()) % 9) - 4;
        levels[band] = static_cast<uint8_t>(std::clamp(value, 2, 100));
    }
    radio_ui_set_audio_levels(levels, kLevelCount);
}

std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~static_cast<std::size_t>(1);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        samples[frame] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

// Out of memory is not the station's fault: it must not count toward skipping
// a healthy station, and retrying at the station back-off rate fragments the heap.
enum class StreamResult { Played, Failed, OutOfMemory };

esp_audio_simple_dec_handle_t open_decoder(esp_audio_simple_dec_type_t type, bool *out_of_memory) {
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = type,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t error = esp_audio_simple_dec_open(&config, &decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "Audio decoder open failed: %d", error);
        *out_of_memory = error == ESP_AUDIO_ERR_MEM_LACK;
        return nullptr;
    }
    return decoder;
}

struct StreamReader {
    radio_stream::Buffer buffer;
    radio_stream::IcyFilter icy;
    RadioSource &source;
    std::size_t station;
    uint32_t generation;
    std::atomic<bool> stop{false}, finished{false};
    std::atomic<uint32_t> max_read_ms{0}, network_bytes{0};
    StreamReader(uint8_t *storage, std::size_t capacity, uint32_t interval,
                 RadioSource &http, std::size_t index, uint32_t version)
        : buffer(storage, capacity), icy(interval), source(http), station(index), generation(version) {}
};

void stream_reader_task(void *arg) {
    auto &reader = *static_cast<StreamReader *>(arg);
    unsigned empty_reads = 0;
    while (!reader.stop.load(std::memory_order_acquire) &&
           request_still_current(reader.generation, reader.station)) {
        std::size_t room = 0;
        uint8_t *target = reader.buffer.write_data(room);
        if (!room) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        const int64_t begin = esp_timer_get_time();
        const int received = reader.source.read(target, std::min<std::size_t>(room, kInputSize));
        const auto elapsed = static_cast<uint32_t>((esp_timer_get_time() - begin) / 1000);
        reader.max_read_ms.store(std::max(reader.max_read_ms.load(), elapsed));
        if (received < 0) {
            ESP_LOGW(kTag, "Stream read error: %d", received);
            break;
        }
        if (!received) {
            if (reader.source.complete() || ++empty_reads > 3) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        empty_reads = 0;
        reader.network_bytes.fetch_add(received, std::memory_order_relaxed);
        reader.buffer.commit(reader.icy.filter(target, received));
    }
    ESP_LOGI(kTag, "HTTP reader stopped. stack_free=%u", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    reader.finished.store(true, std::memory_order_release);
    vTaskSuspend(nullptr);
}

StreamResult stream_station(std::size_t station, uint32_t generation) {
    const RadioStation preset = station_snapshot(station);
    if (!preset.url[0]) {
        ESP_LOGE(kTag, "Station %u has no stream URL", static_cast<unsigned>(station));
        return StreamResult::Failed;
    }
    radio_ui_set_playback(RadioPlaybackState::Connecting, "正在连接电台");

    struct Request { uint32_t generation; std::size_t station; std::atomic<bool> stop{false}; } request{generation, station};
    // Use the unused auxiliary tail rather than fragment AAC's large heap block.
    constexpr size_t scratch_offset = (sizeof(RadioStation) * kMaxStations + 15) & ~size_t(15);
    constexpr size_t scratch_bytes = audio_workspace::radio::kStationBytes - scratch_offset;
    static_assert(scratch_bytes >= RadioSource::kHlsWorkspaceBytes);
    RadioSource source([](void *context) {
        const auto &r = *static_cast<Request *>(context);
        return r.stop.load(std::memory_order_acquire) || !request_still_current(r.generation, r.station);
    }, &request, audio_workspace::radio::stations() + scratch_offset, scratch_bytes);

    bool played_audio = false;
    bool out_of_memory = false;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint8_t *input = nullptr;
    uint8_t *output = nullptr;
    uint8_t *compressed = nullptr;

    do {
        if (!source.open(preset.url)) {
            out_of_memory = source.oom();
            break;
        }
        decoder = open_decoder(source.type(), &out_of_memory);
        if (!decoder) break;
        input = audio_workspace::storage + kStreamStackBytes;
        output = input + kInputSize;

        // The foreground owner already reserved this contiguous FIFO. Do not
        // allocate a 16 KiB heap block after HTTP/MP3 have fragmented the heap.
        constexpr std::size_t capacity = audio_workspace::radio::kCompressedBytes;
        static_assert(capacity <= audio_workspace::kAuxBytes);
        compressed = audio_workspace::aux();
        StreamReader reader(compressed, capacity, source.icy_interval(), source, station, generation);
        TaskHandle_t reader_task = xTaskCreateStatic(stream_reader_task, "radio_http", kReaderStackBytes,
            &reader, 5, reinterpret_cast<StackType_t *>(output + kOutputSize), &s_reader_tcb);
        if (!reader_task) break;
        ESP_LOGI(kTag, "Radio buffer: %u bytes, prefill=%u, icy=%u, free=%u",
            (unsigned)capacity, (unsigned)(capacity / 2), (unsigned)source.icy_interval(),
            (unsigned)esp_get_free_heap_size());

        radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
        bool decode_failed = false;
        bool format_ready = false;
        bool showing_playing = false;
        uint8_t source_channels = 1;
        bool buffering = true;
        uint32_t refills = 0;
        int64_t stats_at = esp_timer_get_time();
        while (request_still_current(generation, station)) {
            const bool finished = reader.finished.load(std::memory_order_acquire);
            const auto available = reader.buffer.available();
            if (finished && !available) break;
            // Begin at the normal refill watermark (8 KiB), retaining the full
            // 16 KiB FIFO and the same recovery reserve after an underrun.
            const auto resume_at = capacity / 2;
            if (buffering && available < resume_at && !finished) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            buffering = false;
            const auto received = reader.buffer.read(input, kInputSize);
            if (!received) {
                buffering = true;
                ++refills;
                showing_playing = false;
                radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
                continue;
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = input,
                .len = static_cast<uint32_t>(received),
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            while (raw.len > 0 && request_still_current(generation, station)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = output,
                    .len = kOutputSize,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                const uint32_t before = raw.len;
                const esp_audio_err_t result =
                    esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGE(kTag, "Decoder needs %u bytes, buffer has %u",
                             static_cast<unsigned>(frame.needed_size),
                             static_cast<unsigned>(kOutputSize));
                    decode_failed = true;
                    raw.len = 0;
                    break;
                }
                if (result == ESP_AUDIO_ERR_MEM_LACK) {
                    // The decoder retries its ~23 KiB init on every block; doing
                    // that per network read shreds the heap. End the session and
                    // let the player's back-off decide when to try again.
                    ESP_LOGW(kTag, "Audio decoder lacks memory; ending session");
                    out_of_memory = true;
                    decode_failed = true;
                    raw.len = 0;
                    break;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(kTag, "Audio decode failed: %d", result);
                    decode_failed = true;
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (!format_ready &&
                        esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                        source_channels = std::clamp<uint8_t>(info.channel, 1, 2);
                        if (info.bits_per_sample != 16 || info.sample_rate < 8000 ||
                            info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) {
                            ESP_LOGE(kTag, "Unsupported stream format: %luHz/%ubit/%uch",
                                     static_cast<unsigned long>(info.sample_rate),
                                     info.bits_per_sample, source_channels);
                            decode_failed = true;
                            raw.len = 0;
                            break;
                        }
                        // Radio and Xiaozhi share one user-visible master volume.
                        // Do not multiply it by the legacy radio NVS volume again.
                        bsp_audio_set_volume(100);
                        ESP_LOGI(kTag, "Radio output: system=%u%%, app=100%%",
                                 static_cast<unsigned>(bsp_audio_get_master_volume()));
                        format_ready = true;
                        ESP_LOGI(kTag, "Playing %s at %luHz/%uch -> mono", preset.name,
                                 static_cast<unsigned long>(info.sample_rate), source_channels);
                    }
                    if (format_ready) {
                        const std::size_t mono_bytes =
                            downmix_to_mono(frame.buffer, frame.decoded_size, source_channels);
                        if (bsp_audio_write(frame.buffer, mono_bytes) != ESP_OK) continue;
                        // Meter updates are visual, not part of every MP3 frame's deadline.
                        static int64_t last_meter_us = 0;
                        const int64_t now = esp_timer_get_time();
                        if (now - last_meter_us >= 50000) {
                            calculate_levels(frame.buffer, mono_bytes, 1);
                            last_meter_us = now;
                        }
                        if (!showing_playing) {
                            played_audio = true;
                            showing_playing = true;
                            radio_ui_set_playback(RadioPlaybackState::Playing, "正在播放");
                        }
                    }
                }

                if (raw.len == before || raw.consumed == 0) {
                    // The parser may cache a short incomplete tail. A fresh network
                    // block is required; keeping this block would spin forever.
                    break;
                }
            }
            if (out_of_memory || decode_failed) break;
            if (esp_timer_get_time() - stats_at >= 10000000) {
                stats_at = esp_timer_get_time();
                ESP_LOGI(kTag, "Radio health: queued=%u/%u refills=%u max_read_ms=%u bytes=%u heap=%u largest=%u",
                    (unsigned)reader.buffer.available(), (unsigned)capacity, (unsigned)refills,
                    (unsigned)reader.max_read_ms.load(), (unsigned)reader.network_bytes.load(),
                    (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            }
        }
        // Never close the HTTP client or release FIFO/stack while a read is in flight.
        reader.stop.store(true, std::memory_order_release);
        request.stop.store(true, std::memory_order_release);
        while (!reader.finished.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(10));
        vTaskDelete(reader_task);
        ESP_LOGI(kTag, "Radio session: refills=%u max_read_ms=%u bytes=%u stack_free=%u",
            (unsigned)refills, (unsigned)reader.max_read_ms.load(),
            (unsigned)reader.network_bytes.load(), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    } while (false);

    // compressed belongs to the shared workspace until the player is stopped.
    if (decoder) esp_audio_simple_dec_close(decoder);
    out_of_memory |= source.oom();
    if (played_audio) return StreamResult::Played;
    return out_of_memory ? StreamResult::OutOfMemory : StreamResult::Failed;
}

void player_task(void *) {
    std::size_t last_attempted_station = SIZE_MAX;
    uint8_t failed_attempts = 0;
    uint8_t auto_skips = 0;
    std::size_t auto_skip_target = SIZE_MAX;
    bool reported_no_network = false;
    bool reported_paused = false;

    while (!s_shutdown_requested.load(std::memory_order_acquire)) {
        if (!s_network_connected.load(std::memory_order_acquire)) {
            if (!reported_no_network) {
                radio_ui_set_playback(RadioPlaybackState::Stopped, "等待网络");
                reported_no_network = true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        reported_no_network = false;

        if (!s_wanted_playing.load(std::memory_order_acquire)) {
            if (!reported_paused) {
                radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
                reported_paused = true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        reported_paused = false;

        const std::size_t station = s_station_index.load(std::memory_order_acquire);
        const uint32_t generation = s_generation.load(std::memory_order_acquire);
        if (station != last_attempted_station) {
            if (station != auto_skip_target) auto_skips = 0; // the user changed station
            last_attempted_station = station;
            failed_attempts = 0;
        }
        s_stream_active.store(true, std::memory_order_release);
        const StreamResult result = stream_station(station, generation);
        s_stream_active.store(false, std::memory_order_release);
        if (!request_still_current(generation, station)) continue;

        if (result == StreamResult::OutOfMemory) {
            // Keep the station and its failure count; give other tasks time
            // to release memory before the next ~23 KiB decoder attempt.
            ESP_LOGW(kTag, "Radio paused for memory; retrying station %u later",
                     static_cast<unsigned>(station));
            radio_ui_set_playback(RadioPlaybackState::Reconnecting,
                                  "内存不足，稍后重试");
            for (unsigned i = 0; i < 100 && request_still_current(generation, station); ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        if (result == StreamResult::Played) {
            // The stream worked at least briefly, so this station is alive and
            // simply dropped. Keep it and reconnect.
            failed_attempts = 0;
            auto_skips = 0;
            radio_ui_set_playback(RadioPlaybackState::Reconnecting,
                                  "信号中断，正在重连");
            for (unsigned i = 0; i < 12 && !s_shutdown_requested.load(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        ++failed_attempts;
        const std::size_t count = radio_player_station_count();
        // Public directories keep dead entries around. After a few silent
        // failures, move on instead of retrying the same address forever.
        if (failed_attempts >= kMaxFailedAttempts && count > 1 && auto_skips < kMaxAutoSkips) {
            ESP_LOGW(kTag, "Station %u unreachable, skipping to the next one",
                     static_cast<unsigned>(station));
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "电台无法播放 已换台");
            for (unsigned i = 0; i < 12 && !s_shutdown_requested.load(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            failed_attempts = 0;
            ++auto_skips;
            radio_player_select_relative(1);
            auto_skip_target = s_station_index.load(std::memory_order_acquire);
            continue;
        }

        // Still retrying on its own: show progress, not a dead end.
        const bool network_suspect = auto_skips >= kMaxAutoSkips;
        radio_ui_set_playback(RadioPlaybackState::Reconnecting,
                              network_suspect ? "网络无法访问电台，重试中" : "连接失败，正在重试");
        for (unsigned i = 0; i < (network_suspect ? 60u : 25u) && request_still_current(generation, station); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    s_stream_active.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Player task stopped");
    if (s_player_stopped != nullptr) xSemaphoreGive(s_player_stopped);
    vTaskSuspend(nullptr);
}

}  // namespace

bool radio_player_init() {
    if (s_player_task.load(std::memory_order_acquire) != nullptr) {
        // A bounded deinit may deliberately leave a slow worker and all of its
        // resources alive.  Do not let the next app entry mistake that
        // shutting-down worker for a reusable player instance.
        return !s_shutdown_requested.load(std::memory_order_acquire);
    }
    if (!audio_workspace::acquire(audio_workspace::Owner::Radio)) {
        ESP_LOGE(kTag, "Audio workspace still owned by another app");
        return false;
    }
    s_shutdown_requested.store(false, std::memory_order_release);
    s_network_connected.store(false, std::memory_order_release);
    s_wanted_playing.store(false, std::memory_order_release);
    s_stream_active.store(false, std::memory_order_release);

    s_station_mutex = xSemaphoreCreateMutex();
    s_player_stopped = xSemaphoreCreateBinary();
    static_assert(kMaxStations * sizeof(RadioStation) <= audio_workspace::radio::kStationBytes);
    static_assert(audio_workspace::radio::kCompressedBytes % alignof(RadioStation) == 0);
    s_stations = reinterpret_cast<RadioStation *>(audio_workspace::radio::stations());
    std::memset(s_stations, 0, kMaxStations * sizeof(RadioStation));
    auto cleanup_failed_init = []() {
        if (s_stations) {
            s_stations = nullptr;
        }
        if (s_player_stopped) {
            vSemaphoreDelete(s_player_stopped);
            s_player_stopped = nullptr;
        }
        if (s_station_mutex) {
            vSemaphoreDelete(s_station_mutex);
            s_station_mutex = nullptr;
        }
        bsp_audio_deinit();
        audio_workspace::release(audio_workspace::Owner::Radio);
        ESP_LOGE(kTag, "Player init failed. free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    };
    if (!s_station_mutex || !s_player_stopped || !s_stations) return cleanup_failed_init();
    std::memcpy(s_stations, kRadioPresets, sizeof(kRadioPresets));
    s_station_count.store(sizeof(kRadioPresets) / sizeof(kRadioPresets[0]));

    const uint8_t saved_station = load_setting("station", 0);
    s_station_index.store(saved_station % radio_player_station_count());

    if (bsp_audio_init_playback() != ESP_OK) return cleanup_failed_init();
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
        bsp_audio_deinit();
        return cleanup_failed_init();
    }
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        esp_audio_dec_unregister_default();
        bsp_audio_deinit();
        return cleanup_failed_init();
    }
    s_decoders_registered = true;
    bsp_audio_set_volume(100);

    radio_ui_set_station(s_station_index.load(), radio_player_station_count(),
                         radio_player_station(s_station_index.load()));
    radio_ui_set_volume(radio_player_volume());
    TaskHandle_t task = xTaskCreateStatic(player_task, "radio_stream", kStreamStackBytes, nullptr, 6,
        reinterpret_cast<StackType_t *>(audio_workspace::storage), &s_stream_tcb);
    if (task == nullptr) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        s_decoders_registered = false;
        bsp_audio_deinit();
        return cleanup_failed_init();
    }
    s_player_task.store(task, std::memory_order_release);
    ESP_LOGI(kTag, "Player ready. free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

bool radio_player_deinit(uint32_t timeout_ms) {
    s_wanted_playing.store(false, std::memory_order_release);
    s_network_connected.store(false, std::memory_order_release);
    s_shutdown_requested.store(true, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);

    if (s_player_task.load(std::memory_order_acquire) != nullptr) {
        if (s_player_stopped == nullptr ||
            xSemaphoreTake(s_player_stopped, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            if (timeout_ms) ESP_LOGE(kTag, "Player task did not stop in %ums; keeping resources alive",
                     (unsigned)timeout_ms);
            return false;
        }
    }

    // The worker reports completion and suspends. Delete it before allowing
    // the next application to use its stack/buffers in the shared workspace.
    if (auto task = s_player_task.exchange(nullptr)) vTaskDelete(task);
    if (s_decoders_registered) {
        esp_audio_simple_dec_unregister_default();
        esp_audio_dec_unregister_default();
        s_decoders_registered = false;
    }
    bsp_audio_deinit();
    if (s_player_stopped) {
        vSemaphoreDelete(s_player_stopped);
        s_player_stopped = nullptr;
    }
    if (s_station_mutex) {
        vSemaphoreDelete(s_station_mutex);
        s_station_mutex = nullptr;
    }
    if (s_stations) {
        s_stations = nullptr;
    }
    s_station_count.store(0, std::memory_order_release);
    audio_workspace::release(audio_workspace::Owner::Radio);
    ESP_LOGI(kTag, "Player deinit complete. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

void radio_player_set_network(bool connected) {
    s_network_connected.store(connected, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void radio_player_toggle() {
    radio_player_set_playing(!radio_player_is_playing());
}

void radio_player_set_playing(bool playing) {
    s_wanted_playing.store(playing, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (!playing) radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
}

bool radio_player_is_playing() {
    return s_wanted_playing.load(std::memory_order_acquire);
}

bool radio_player_wait_idle(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_stream_active.load(std::memory_order_acquire)) {
        if (static_cast<int32_t>(deadline - xTaskGetTickCount()) <= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void radio_player_select_relative(int delta) {
    const int count = static_cast<int>(radio_player_station_count());
    const int current = static_cast<int>(s_station_index.load());
    const std::size_t next = static_cast<std::size_t>((current + delta + count) % count);
    radio_player_set_station(next);
}

void radio_player_set_station(std::size_t index) {
    index %= radio_player_station_count();
    s_station_index.store(index, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    save_setting("station", static_cast<uint8_t>(index));
    const RadioStation chosen = radio_player_station(index);
    save_station_name(chosen.name);
    radio_ui_set_station(index, radio_player_station_count(), radio_player_station(index));
    if (radio_player_is_playing()) {
        radio_ui_set_playback(RadioPlaybackState::Connecting, "正在切换电台");
    }
}

std::size_t radio_player_station_index() {
    return s_station_index.load(std::memory_order_acquire);
}

std::size_t radio_player_station_count() {
    return std::max<std::size_t>(s_station_count.load(std::memory_order_acquire), 1);
}

RadioStation radio_player_station(std::size_t index) {
    return station_snapshot(index);
}

bool radio_player_replace_stations(const RadioStation *stations, std::size_t count) {
    std::size_t selected = 0;
    if (!install_stations(stations, count, nullptr, &selected)) return false;

    save_setting("station", static_cast<uint8_t>(selected));
    const RadioStation chosen = radio_player_station(selected);
    save_station_name(chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    ESP_LOGI(kTag, "Installed local catalog with %u stations",
             static_cast<unsigned>(radio_player_station_count()));
    return true;
}

bool radio_player_replace_stations_resuming(const RadioStation *stations,
                                           std::size_t count) {
    char remembered[sizeof(RadioStation::name)] = {};
    const bool has_remembered = load_station_name(remembered, sizeof(remembered));

    std::size_t selected = 0;
    if (!install_stations(stations, count, has_remembered ? remembered : nullptr,
                          &selected)) {
        return false;
    }

    save_setting("station", static_cast<uint8_t>(selected));
    const RadioStation chosen = radio_player_station(selected);
    save_station_name(chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    if (has_remembered && selected > 0) {
        ESP_LOGI(kTag, "Resumed remembered station '%s' at index %u",
                 chosen.name, static_cast<unsigned>(selected));
    } else {
        ESP_LOGI(kTag, "Installed local catalog with %u stations",
                 static_cast<unsigned>(radio_player_station_count()));
    }
    return true;
}

void radio_player_set_volume(uint8_t volume) {
    settings_set_volume(volume);
    radio_ui_set_volume(radio_player_volume());
}

uint8_t radio_player_volume() {
    return bsp_audio_get_master_volume();
}
