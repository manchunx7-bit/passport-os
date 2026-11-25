#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace podcast {

constexpr unsigned kPageSize = 3;
struct Program { uint32_t id; const char *title; };
inline constexpr Program kPrograms[] = {
    {51076156, "声动早咖啡"}, {5574153, "日谈公园"},
    {35417311, "BBC 随身英语"}, {82749641, "TED 演讲精选"},
};
struct Episode {
    uint32_t id;
    uint32_t duration;
    bool paid;
    char title[112];
    char url[256];
    char backup_url[256];
};
struct Page {
    unsigned count;
    unsigned total;
    Episode episodes[kPageSize];
};

bool fetch_page(uint32_t album, unsigned page, const std::atomic<bool> &cancel, Page &result);
// Book* channels feed the standalone 听书 app; Classics merges two searches.
enum class Discovery : uint8_t { Hot, Recommended, Business, Story, Crosstalk, Search,
                                 BookHot, BookMystery, BookHistory, BookClassics };
constexpr unsigned kDiscoveryLimit = 8;
struct Listing {
    uint32_t id;
    uint32_t plays;
    uint32_t tracks;
    char title[96];
};
struct ProgramList {
    unsigned count;
    Listing items[kDiscoveryLimit];
};

bool discover(Discovery source, const char *query, const std::atomic<bool> &cancel, ProgramList &result);
const char *recommended_theme_name();
// Returns count when no title matches. Exact matches take precedence.
unsigned match_program(const ProgramList &list, const char *query);
// Spoken book titles rarely match decorated album names exactly: exact first,
// otherwise the most played title containing the query. Count when none.
unsigned match_book(const ProgramList &list, const char *query);
bool audio_url(const char *input, char *output, std::size_t capacity);

} // namespace podcast
