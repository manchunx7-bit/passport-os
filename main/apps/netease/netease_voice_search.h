// Song search while Xiaozhi is still connected, so the assistant can name the
// result or ask which version before the music app takes over.
#pragma once

#include "apps/netease/netease_api.h"

#include <cstddef>
#include <cstdint>

namespace netease {

constexpr size_t kVoiceSearchResults = 5;

// Blocks the caller for at most timeout_ms. NoMemory when the heap cannot hold
// a search task plus an HTTP connection beside the Xiaozhi session, Cancelled on
// timeout or while another voice search is still finishing.
Result voice_search(const char *query, Song *out, size_t *count, uint32_t timeout_ms);

// Consume recent results after handover (also across a clean-launch restart).
// Cache misses keep the music app's normal network search as a fallback.
bool take_voice_search(const char *query, Song *out, uint64_t *ids, size_t capacity, size_t *count);

}  // namespace netease
