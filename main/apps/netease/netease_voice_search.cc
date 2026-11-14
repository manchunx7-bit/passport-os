#include "apps/netease/netease_voice_search.h"
#include "apps/netease/netease_voice_cache.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <new>

namespace netease {
namespace {

constexpr char kTag[] = "netease_voice";
// search_songs needs ~3 KB of stack inside the HTTP client. A task of its own
// keeps that off the Xiaozhi control task, and its stack is freed afterwards.
constexpr uint32_t kTaskStack = 6144;
// Task stack, HTTP client buffers, socket and parser headroom.
constexpr size_t kNeedFree = 24 * 1024;
constexpr size_t kNeedLargest = 8 * 1024;
RTC_NOINIT_ATTR VoiceSearchCache s_search_cache;
static_assert(kVoiceSearchResults == VoiceSearchCache::kCount);

// One search at a time; a timed-out search may still be closing its socket.
std::atomic<bool> s_busy{false};
std::atomic<bool> s_cancel{false};
bool cancelled() { return s_cancel.load(std::memory_order_acquire); }

// Shared by the caller and the task. Whichever finishes second frees it, so a
// caller that timed out never waits for, or frees under, a running search.
struct Job {
    char query[64];
    Song songs[kVoiceSearchResults];
    uint64_t ids[kVoiceSearchResults];
    size_t count = 0;
    Result result = Result::Network;
    std::atomic<bool> done{false};
    std::atomic<bool> released{false};
};

void release(Job *job) {
    if (job->released.exchange(true, std::memory_order_acq_rel)) delete job;
}

void search_task(void *arg) {
    auto *job = static_cast<Job *>(arg);
    job->result = search_songs(job->query, job->songs, job->ids, kVoiceSearchResults, &job->count, cancelled);
    job->done.store(true, std::memory_order_release);
    release(job);
    s_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

}  // namespace

Result voice_search(const char *query, Song *out, size_t *count, uint32_t timeout_ms) {
    *count = 0;
    if (!query || !query[0]) return Result::Api;
    if (esp_get_free_heap_size() < kNeedFree || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kNeedLargest) {
        ESP_LOGW(kTag, "Skip search: free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return Result::NoMemory;
    }
    bool idle = false;
    if (!s_busy.compare_exchange_strong(idle, true)) return Result::Cancelled;
    auto *job = new (std::nothrow) Job;
    if (!job) { s_busy.store(false); return Result::NoMemory; }
    std::snprintf(job->query, sizeof(job->query), "%s", query);
    s_cancel.store(false, std::memory_order_release);
    if (xTaskCreate(search_task, "music_search", kTaskStack, job, 4, nullptr) != pdPASS) {
        delete job;
        s_busy.store(false);
        return Result::NoMemory;
    }
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (!job->done.load(std::memory_order_acquire) && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    Result result = Result::Cancelled;
    if (job->done.load(std::memory_order_acquire)) {
        result = job->result;
        *count = job->count;
        for (size_t i = 0; i < job->count; ++i) out[i] = job->songs[i];
        if (result == Result::Ok) s_search_cache.save(query, out, *count, esp_timer_get_time());
    } else {
        s_cancel.store(true, std::memory_order_release);  // the task ends its request and frees the job
        ESP_LOGW(kTag, "Search timed out after %u ms", (unsigned)timeout_ms);
    }
    release(job);
    return result;
}

bool take_voice_search(const char *query, Song *out, uint64_t *ids, size_t capacity, size_t *count) {
    return s_search_cache.take(query, out, ids, capacity, count, esp_timer_get_time(),
                               esp_reset_reason() == ESP_RST_SW);
}

}  // namespace netease
