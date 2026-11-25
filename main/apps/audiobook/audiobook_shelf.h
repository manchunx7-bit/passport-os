#pragma once

#include <cstddef>
#include <cstdint>

namespace audiobook {

constexpr size_t kShelfCapacity = 5;

struct ShelfItem {
    uint32_t chapter = 0;
    uint32_t tracks = 0;
    bool finished = false;
    char title[96] = {};
};

size_t read_shelf_summary(ShelfItem *out, size_t cap);

}  // namespace audiobook
