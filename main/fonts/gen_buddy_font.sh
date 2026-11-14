#!/usr/bin/env bash
set -euo pipefail

PROJ="/mnt/f/WORK/AI硬件/passport-os"
OTF="$PROJ/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
SYMBOLS_FILE="$PROJ/main/fonts/buddy_16_symbols.txt"
NAME_SYMBOLS_FILE="$PROJ/main/fonts/buddy_16_name_symbols.txt"
CITY_SYMBOLS_FILE="$PROJ/main/fonts/city_symbols.txt"
# Common 2nd-level characters, chat particles and punctuation for spoken replies
# (selection recorded in docs/font-expand-20261108.md).
CHAT_SYMBOLS_FILE="$PROJ/main/fonts/buddy_16_chat_symbols.txt"
OUT="$PROJ/main/fonts/buddy_font_16.c"

echo "=== Generating buddy_font_16.c ==="
echo "OTF: $OTF"
echo "Base symbols count: $(tr -d '\r\n' < "$SYMBOLS_FILE" | wc -m)"
echo "Name symbols count: $(tr -d '\r\n' < "$NAME_SYMBOLS_FILE" | wc -m)"

BASE_SYMBOLS=$(cat "$SYMBOLS_FILE")
NAME_SYMBOLS=$(cat "$NAME_SYMBOLS_FILE")
CITY_SYMBOLS=$(cat "$CITY_SYMBOLS_FILE")
CHAT_SYMBOLS=$(cat "$CHAT_SYMBOLS_FILE")
SYMBOLS="${BASE_SYMBOLS}${NAME_SYMBOLS}${CITY_SYMBOLS}${CHAT_SYMBOLS}"

npx --yes lv_font_conv \
  --font "$OTF" \
  --range 0x20-0x7e \
  --symbols "$SYMBOLS" \
  --size 16 --bpp 2 --format lvgl --no-compress --no-prefilter --no-kerning \
  --lv-include lvgl.h --lv-font-name buddy_font_16 \
  -o "$OUT"

echo "Generated buddy_font_16.c successfully: $(ls -lh "$OUT")"
