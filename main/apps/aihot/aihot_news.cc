#include "apps/aihot/aihot_news.h"

#include "apps/aihot/aihot_format.h"
#include "apps/netease/netease_catalog.h"
#include "apps/xiaozhi/xiaozhi_store.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <new>

namespace aihot {
namespace {

constexpr char kTag[] = "aihot";
constexpr char kNs[] = "aihot";
// TLS handshake (certificate verify) plus the scanner and collector frames.
constexpr uint32_t kTaskStack = 7168;
// The Xiaozhi session keeps ~72 KiB free (largest ~61 KiB, measured): a second
// TLS session needs its 8 KiB record buffer contiguous plus handshake scratch.
constexpr size_t kNeedFree = 40 * 1024;
constexpr size_t kNeedLargest = 16 * 1024;
constexpr int64_t kRequestDeadlineUs = 9 * 1000 * 1000;

std::atomic<bool> s_busy{false};
std::atomic<bool> s_cancel{false};

// Shared by caller and task; whichever finishes second frees it.
struct Job {
    Query query;
    char text[kTextBytes] = {};
    Result result = Result::Network;
    std::atomic<bool> done{false};
    std::atomic<bool> released{false};
};

void release(Job *job) {
    if (job->released.exchange(true, std::memory_order_acq_rel)) delete job;
}

// AIHOT asks clients for an anonymous actor id in the User-Agent. Like the
// browser, each device makes its own random one; no account is involved.
void user_agent(char *out, size_t capacity) {
    std::string actor = store_get_string(kNs, "actor", "");
    if (actor.size() != 36) {
        uint8_t b[16];
        for (int i = 0; i < 16; i += 4) {
            const uint32_t r = esp_random();
            std::memcpy(b + i, &r, 4);
        }
        b[6] = (b[6] & 0x0F) | 0x40;  // UUID version 4
        b[8] = (b[8] & 0x3F) | 0x80;
        char id[37];
        std::snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        actor = id;
        store_set_string(kNs, "actor", actor);
    }
    std::snprintf(out, capacity, "aihot-api/2.0.0 aihot-actor/%s", actor.c_str());
}

bool url(const Query &q, char *out, size_t capacity) {
    const unsigned limit = q.kind == Kind::Hot ? (q.limit ? q.limit : kMaxHot) : (q.limit ? q.limit : 5);
    int n = 0;
    if (q.kind == Kind::Hot) n = std::snprintf(out, capacity, "https://aihot.news/api/v1/hot-topics");
    else if (q.kind == Kind::Daily) n = std::snprintf(out, capacity, "https://aihot.news/api/v1/dailies/latest");
    else {
        // Keywords search all items, by default over the week; the latest
        // selection defaults to the past day.
        char keywords[3 * 63 + 1] = "";
        if (q.text[0] && !netease::url_encode(q.text, keywords, sizeof(keywords))) return false;
        const bool week = q.days ? q.days == 7 : q.text[0] != 0;
        n = std::snprintf(out, capacity, "https://aihot.news/api/v1/items?mode=%s&window=%s&limit=%u%s%s%s%s",
                          q.text[0] ? "all" : "selected", week ? "7d" : "24h", limit,
                          q.category[0] ? "&category=" : "", q.category, q.text[0] ? "&q=" : "", keywords);
    }
    return n > 0 && static_cast<size_t>(n) < capacity;
}

Result get(Job &job) {
    const Query &q = job.query;
    char address[320];
    if (!url(q, address, sizeof(address))) return Result::Parse;
    char agent[96];
    user_agent(agent, sizeof(agent));
    const unsigned limit = q.limit ? q.limit : (q.kind == Kind::Hot ? kMaxHot : 5);
    Writer out{job.text, sizeof(job.text)};
    HotCollector hot{out, limit};
    NewsCollector news{out, limit};
    DailyCollector daily{out};
    JsonScanner scanner(q.kind == Kind::Hot ? HotCollector::sink : q.kind == Kind::News ? NewsCollector::sink : DailyCollector::sink,
                        q.kind == Kind::Hot ? static_cast<void *>(&hot) : q.kind == Kind::News ? static_cast<void *>(&news)
                                                                                                : static_cast<void *>(&daily));
    esp_http_client_config_t cfg{};
    cfg.url = address;
    cfg.timeout_ms = 6000;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 512;
    cfg.user_agent = agent;
    cfg.disable_auto_redirect = true;
    cfg.keep_alive_enable = false;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return Result::NoMemory;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    Result result = Result::Network;
    const int64_t deadline = esp_timer_get_time() + kRequestDeadlineUs;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0) break;
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(kTag, "HTTP %d", status);
            result = status == 429 ? Result::RateLimited : Result::Http;
            break;
        }
        char chunk[512];
        for (;;) {
            if (s_cancel.load(std::memory_order_acquire) || esp_timer_get_time() > deadline) { result = Result::Timeout; break; }
            const int n = esp_http_client_read(client, chunk, sizeof(chunk));
            if (n < 0) break;
            if (n == 0) {
                result = esp_http_client_is_complete_data_received(client) && scanner.complete() ? Result::Ok
                         : scanner.failed() ? Result::Parse : Result::Network;
                break;
            }
            if (!scanner.feed(chunk, static_cast<size_t>(n))) { result = Result::Parse; break; }
        }
    } while (false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result == Result::Ok && !out.entries) result = Result::Empty;
    return result;
}

void task(void *arg) {
    auto *job = static_cast<Job *>(arg);
    job->result = get(*job);
    job->done.store(true, std::memory_order_release);
    release(job);
    s_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

}  // namespace

Result fetch(const Query &query, std::string *text, uint32_t timeout_ms) {
    text->clear();
    if (esp_get_free_heap_size() < kNeedFree || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kNeedLargest) {
        ESP_LOGW(kTag, "Skip fetch: free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return Result::NoMemory;
    }
    bool idle = false;
    if (!s_busy.compare_exchange_strong(idle, true)) return Result::Busy;
    auto *job = new (std::nothrow) Job;
    if (!job) { s_busy.store(false); return Result::NoMemory; }
    job->query = query;
    s_cancel.store(false, std::memory_order_release);
    if (xTaskCreate(task, "aihot", kTaskStack, job, 4, nullptr) != pdPASS) {
        delete job;
        s_busy.store(false);
        return Result::NoMemory;
    }
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (!job->done.load(std::memory_order_acquire) && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    Result result = Result::Timeout;
    if (job->done.load(std::memory_order_acquire)) {
        result = job->result;
        if (result == Result::Ok) text->assign(job->text);
    } else {
        s_cancel.store(true, std::memory_order_release);  // the task ends its request and frees the job
        ESP_LOGW(kTag, "Fetch timed out after %u ms", (unsigned)timeout_ms);
    }
    release(job);
    return result;
}

}  // namespace aihot
