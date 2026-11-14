#!/usr/bin/env bash
# Render the Xiaozhi face contact sheets with the project's own LVGL sources.
#   bash tools/xiaozhi_face_preview.sh [out_dir]
# LVGL is compiled once into /tmp/passport-lvgl-host and reused.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-output/xiaozhi-face}"
lib=/tmp/passport-lvgl-host
lvgl=managed_components/lvgl__lvgl
mkdir -p "$out" "$lib/obj"

if [ ! -f "$lib/lv_conf.h" ]; then
    sed -e 's/^#if 0 .*Set this to "1" to enable content.*/#if 1/' \
        -e 's/^#define LV_COLOR_DEPTH .*/#define LV_COLOR_DEPTH 16/' \
        -e 's/^    #define LV_MEM_SIZE .*/    #define LV_MEM_SIZE (1024 * 1024U)/' \
        "$lvgl/lv_conf_template.h" > "$lib/lv_conf.h"
fi
flags=(-O1 -DLV_CONF_INCLUDE_SIMPLE "-I$lib" "-I$lvgl")
if [ ! -f "$lib/liblvgl.a" ]; then
    find "$lvgl/src" -name '*.c' | while read -r src; do
        obj="$lib/obj/$(echo "$src" | tr '/' '_').o"
        echo "cc ${flags[*]} -c $src -o $obj"
    done | xargs -P "$(nproc)" -I{} sh -c '{}'
    ar rcs "$lib/liblvgl.a" "$lib"/obj/*.o
fi
c++ -std=c++17 -Wall -Wextra -Werror "${flags[@]}" -Imain \
    tools/xiaozhi_face_preview.cc main/apps/xiaozhi/xiaozhi_face.cc "$lib/liblvgl.a" -lm -o "$lib/face-preview"
"$lib/face-preview" "$out" "${2:-}"
