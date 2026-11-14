#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 tests/test_voice_memory.py
python3 tests/test_xiaozhi_face_power.py
python3 tests/check_digit_font.py
python3 tests/check_title_glyphs.py
python3 tests/check_launcher_apps.py
test_dir="$(mktemp -d /tmp/passport-host.XXXXXX)"
trap 'rm -rf -- "$test_dir"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Imain tests/test_xiaozhi_level.cc -o "$test_dir/xiaozhi-level"
"$test_dir/xiaozhi-level"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable \
    -fsanitize=address,undefined -g -Itests/registry_mocks -Imain \
    tests/test_app_registry.cc -o "$test_dir/registry"
"$test_dir/registry"
python3 tools/generate_ui_sfx.py --check
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/game_audio_mocks -Imain tests/test_ui_sfx.c -lm -o "$test_dir/ui-sfx"
"$test_dir/ui-sfx"
for mode in MIC SFX; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -DTEST_$mode -Itests/game_audio_mocks -Imain tests/test_game_audio_lifecycle.c -o "$test_dir/game-$mode"
    "$test_dir/game-$mode"
done
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain/apps/voice \
    tests/test_ui_pixel_math.c main/apps/voice/ui_pixel_math.c -o "$test_dir/math"
"$test_dir/math"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_tetris.c main/apps/game/tetris_core.c -o "$test_dir/tetris"
"$test_dir/tetris"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Imain \
    tests/test_terminal.cc -o "$test_dir/terminal"
"$test_dir/terminal"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/audio_mocks -Icomponents/bsp/src \
    tests/test_audio_volume.c components/bsp/src/bsp_audio_volume.c -lm -o "$test_dir/audio-volume"
"$test_dir/audio-volume"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_stopwatch.c main/apps/stopwatch/stopwatch_core.c -o "$test_dir/stopwatch"
"$test_dir/stopwatch"
python3 tests/test_wifi_configure.py
python3 tests/test_wifi_internet.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_battery_guard.c -o "$test_dir/battery-guard"
"$test_dir/battery-guard"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_pomodoro.c main/apps/pomodoro/pomodoro_core.c -o "$test_dir/pomodoro"
"$test_dir/pomodoro"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    -c main/apps/pomodoro/pomodoro_core.c -o "$test_dir/pomodoro-core.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    -c main/apps/stopwatch/stopwatch_core.c -o "$test_dir/stopwatch-core.o"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Imain \
    tests/test_timer_resume.cc "$test_dir/stopwatch-core.o" "$test_dir/pomodoro-core.o" -o "$test_dir/timer-resume"
"$test_dir/timer-resume"
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -Wno-unused-variable -Wno-misleading-indentation \
    -fsanitize=address,undefined -g -Itests/pomodoro_mocks -Imain -Imain/fonts \
    tests/test_pomodoro_app.cc "$test_dir/pomodoro-core.o" -o "$test_dir/pomodoro-app"
"$test_dir/pomodoro-app"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Imain \
    tests/test_wifi_keyboard.cc -o "$test_dir/wifi-keyboard"
"$test_dir/wifi-keyboard"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -Wno-sign-compare \
    -Wno-unused-parameter -Wno-misleading-indentation -fsanitize=address,undefined -g \
    -Itests/wifi_mocks -Icomponents/esp-wifi-connect/include \
    tests/test_wifi_station.cc -o "$test_dir/wifi-station"
"$test_dir/wifi-station"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain -Icomponents/esp-wifi-connect/include \
    tests/test_captive_setup.cc -o "$test_dir/captive-setup"
"$test_dir/captive-setup"
node tests/test_profile_page.js
node tests/test_portrait_pixels.js
python3 tests/test_card_gallery.py
python3 tests/test_avatar_upload.py
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-variable \
    -fsanitize=address,undefined -g -ffunction-sections -fdata-sections \
    -Itests/home_mocks -Itests/radio_mocks -Imain \
    tests/test_home_storage.c -Wl,--gc-sections -lm -o "$test_dir/home-storage"
"$test_dir/home-storage"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/vendor/cjson -Icomponents/esp-wifi-connect/include \
    tests/test_phone_time.cc tests/vendor/cjson/cJSON.c -o "$test_dir/phone-time"
"$test_dir/phone-time"
python3 tests/test_city_catalog.py
python3 tests/test_home_weather.py
python3 tests/test_home_weather_schedule.py
python3 tests/test_radio_catalog.py
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Imain tests/test_radio_hls.cc -o "$test_dir/radio-hls"
"$test_dir/radio-hls"
python3 tests/test_radio_source.py
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -g -Imain tests/test_radio_stream_buffer.cc -o "$test_dir/radio-buffer"
"$test_dir/radio-buffer"
python3 tests/test_xiaozhi.py
python3 tests/test_reminders.py
python3 tests/test_device_control.py
python3 tests/test_lists.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/passport_muse/sdk -Icomponents/passport_muse/include \
    tests/test_muse_voice.c components/passport_muse/voice_helpers.c -o "$test_dir/muse-voice"
python3 tests/test_muse_chat_link.py
python3 tests/test_muse_pairing_dispatch.py
python3 tests/test_muse_pairing_memory.py
python3 tests/test_muse_pairing_decrypt.py
python3 tests/test_muse_pairing_base64.py
python3 tests/test_muse_workspace.py
python3 tests/test_muse_readiness.py
python3 tests/test_muse_tls_reservation.py
python3 tests/test_muse_tls_memory.py
python3 tests/test_input_serial_dispatch.py
python3 tests/test_button_poll.py
python3 tests/test_bsp_button.py
python3 tests/test_muse_queue.py
python3 tests/test_bsp_audio_profiles.py
python3 tests/test_memory_telemetry.py
python3 tests/test_radio_lifecycle.py
python3 tests/test_home_portrait_lifecycle.py
python3 tests/test_voice_startup.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Itests/home_mocks -Itests/radio_mocks -Icomponents/passport_muse/sdk \
    tests/test_muse_config.c components/passport_muse/config_store.c -o "$test_dir/muse-config"
"$test_dir/muse-config"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    -Icomponents/passport_muse/sdk \
    tests/test_muse_proxy_route.c components/passport_muse/sdk/proxy_route.c -o "$test_dir/muse-proxy-route"
"$test_dir/muse-proxy-route"
"$test_dir/muse-voice"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_sound_jump.c main/apps/sound_jump/jump_core.c main/apps/sound_jump/jump_voice.c -o "$test_dir/sound-jump"
"$test_dir/sound-jump"
python3 tests/test_verify_firmware.py
python3 tests/test_public_build.py
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Imain \
    tests/test_voice_remote.c main/apps/voice/app_state.c -o "$test_dir/voice-remote"
"$test_dir/voice-remote"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itests/battery_mocks -Icomponents/bsp/include \
    tests/test_battery_driver.c components/bsp/src/bsp_battery.c -o "$test_dir/battery"
"$test_dir/battery"
python3 tests/test_netease_parse.py
python3 tests/test_media_startup.py
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/registry_mocks -Imain tests/test_netease_controls.cc -o "$test_dir/netease-controls"
"$test_dir/netease-controls"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/registry_mocks -Imain \
    tests/test_netease_intent.cc main/apps/netease/netease_json.cc -o "$test_dir/netease-intent"
"$test_dir/netease-intent"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain \
    tests/test_aihot.cc main/apps/netease/netease_json.cc -o "$test_dir/aihot"
"$test_dir/aihot"
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain     tests/test_netease_dsp.cc -o "$test_dir/netease-dsp"
"$test_dir/netease-dsp"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain \
    -Itests/registry_mocks tests/test_netease_performance.cc main/apps/netease/netease_json.cc -o "$test_dir/netease-performance"
"$test_dir/netease-performance"
echo "Host regression tests: PASS"
