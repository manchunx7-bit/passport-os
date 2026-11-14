// AIHOT (aihot.news) AI news for Xiaozhi, fetched by the device itself.
//
// The public REST API is HTTPS only; its server sends ~4 KB TLS records, well
// inside this device's 8 KB receive buffer. Each fetch runs on a short-lived
// task with its own stack, beside the live Xiaozhi session, and turns the JSON
// reply into at most kTextBytes of text.
#pragma once

#include <cstdint>
#include <string>

namespace aihot {

enum class Kind : uint8_t { Hot, News, Daily };
enum class Result : uint8_t { Ok, Empty, NoMemory, Busy, Timeout, Network, Http, RateLimited, Parse };

struct Query {
    Kind kind = Kind::Hot;
    unsigned limit = 0;        // 0: the kind's default
    char text[64] = {};        // News: keywords (all items, not only the selection)
    char category[16] = {};    // News: ai-models, ai-products, industry, paper, tip, opinion
    uint8_t days = 0;          // News: 1 or 7; 0 = 7 with keywords, else 1
};

// Blocks the caller for at most timeout_ms; text receives the formatted lines.
Result fetch(const Query &query, std::string *text, uint32_t timeout_ms);

}  // namespace aihot
