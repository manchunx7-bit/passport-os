#include "voice_intent.h"

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace voice_intent {
namespace {

constexpr uint32_t kMagic = 0x564e5432;  // "VNT2"

// RTC memory: survives the clean-launch esp_restart() like app_registry's
// target record, and costs nothing from the main heap region.
// Arm and clear run on the input task, which also performs the switch; take
// runs in the app start that switch (or the boot after a clean launch) makes.
struct Stored {
    uint32_t magic;
    uint32_t check;
    int64_t armed_us;
    Request request;
};
RTC_NOINIT_ATTR Stored s_stored;

uint32_t checksum(const Stored &stored) {
    uint32_t hash = 2166136261u ^ kMagic;
    const auto mix = [&hash](const void *data, size_t size) {
        for (size_t i = 0; i < size; ++i) hash = (hash ^ static_cast<const uint8_t *>(data)[i]) * 16777619u;
    };
    mix(&stored.armed_us, sizeof(stored.armed_us));
    mix(&stored.request, sizeof(stored.request));
    return hash;
}

}  // namespace

void arm(const Request &request) {
    s_stored.magic = 0;
    s_stored.request = request;
    s_stored.request.name[kNameBytes - 1] = 0;
    s_stored.request.detail[kDetailBytes - 1] = 0;
    s_stored.armed_us = esp_timer_get_time();
    s_stored.check = checksum(s_stored);
    s_stored.magic = kMagic;
}

void clear() { s_stored.magic = 0; }

Request take(passport_app_id_t app) {
    // Power-on leaves RTC memory undefined: require the magic and checksum.
    const bool valid = s_stored.magic == kMagic && s_stored.check == checksum(s_stored) &&
                       s_stored.request.source != Source::None && s_stored.request.app == app;
    Request request = valid ? s_stored.request : Request{};
    const bool usable = valid && fresh(s_stored.armed_us, esp_timer_get_time(), esp_reset_reason() == ESP_RST_SW);
    clear();  // a request for another app is stale once any media app starts
    if (!usable) return {};
    request.name[kNameBytes - 1] = 0;
    request.detail[kDetailBytes - 1] = 0;
    return request;
}

}  // namespace voice_intent
