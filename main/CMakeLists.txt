option(PASSPORT_MUSE_USE_JOLLYBOT "Build Muse with the official Jollybot artwork" ON)
if(PASSPORT_MUSE_USE_JOLLYBOT)
    set(PASSPORT_MUSE_AVATAR "apps/muse/muse_avatar_frames.c")
else()
    set(PASSPORT_MUSE_AVATAR "apps/muse/muse_avatar_public_frames.c")
endif()
idf_component_register(
    EMBED_FILES "apps/home/cities.json.gz"
    SRCS
        "main.cc"
        "device_control.cc"
        "device_control_mcp.cc"
        "voice_intent.cc"
        "apps/lists/list_service.cc"
        "apps/lists/list_mcp.cc"
        "apps/lists/app_lists.cc"
        "apps/aihot/aihot_news.cc"
        "apps/aihot/aihot_mcp.cc"
        "apps/reminders/reminder_core.cc"
        "apps/reminders/reminder_service.cc"
        "apps/reminders/reminder_mcp.cc"
        "apps/reminders/app_reminders.cc"
        "apps/xiaozhi/opus_encoder_storage.cc"
        "boot_chime.c"
        "ui_sfx.c"
        "ui_sfx_tone.c"
        "boot_splash.c"
        "launcher/launcher.cc"
        "launcher/app_registry.cc"
        "launcher/wifi_setup.cc"
        "launcher/wifi_picker.cc"
        "launcher/network_diagnostics.cc"
        "launcher/timer_resume.cc"
        "launcher/settings.cc"
        "launcher/power.cc"
        "launcher/system_overlay.cc"
        "fonts/buddy_font_16.c"
        "fonts/ui_font_20.c"
        "fonts/ui_digits_32.c"
        "apps/podcast/app_podcast.cc"
        "apps/podcast/podcast_catalog.cc"
        "apps/audiobook/app_audiobook.cc"
        "apps/radio/app_radio.cc"
        "apps/radio/radio_catalog.cc"
        "apps/radio/radio_display_settings.cc"
        "apps/radio/radio_player.cc"
        "apps/radio/radio_source.cc"
        "apps/radio/radio_sleep_timer.cc"
        "apps/radio/radio_ui.cc"
        "apps/voice/app_voice.cc"
        "apps/voice/adpcm.c"
        "apps/voice/audio_streamer.c"
        "apps/voice/udp_audio.c"
        "apps/voice/app_protocol.c"
        "apps/voice/app_sound.c"
        "apps/voice/app_events.c"
        "apps/voice/app_ui.c"
        "apps/voice/app_state.c"
        "apps/voice/time_sync.c"
        "ui/ui_pixel.c"
        "ui/ui_portrait.c"
        "ui/ui_capture.c"
        "apps/voice/ui_pixel_math.c"
        "apps/voice/mode.c"
        "apps/voice/nvs_settings.c"
        "apps/xiaozhi/xiaozhi_protocol.cc"
        "apps/xiaozhi/xiaozhi_audio.cc"
        "apps/xiaozhi/xiaozhi_ui.cc"
        "apps/xiaozhi/xiaozhi_face.cc"
        "apps/xiaozhi/xiaozhi_store.cc"
        "apps/xiaozhi/xiaozhi_ota.cc"
        "apps/xiaozhi/app_xiaozhi.cc"
        "apps/netease/app_netease.cc"
        "apps/netease/netease_api.cc"
        "apps/netease/netease_voice_search.cc"
        "apps/netease/netease_json.cc"
        "apps/netease/netease_lyrics.cc"
        "apps/netease/netease_player.cc"
        "apps/netease/netease_ui.cc"
        "apps/home/home_profile.c"
        "apps/home/home_clock.c"
        "apps/home/home_weather.c"
        "apps/home/band_art.c"
        "apps/home/home_upload.cc"
        "apps/home/app_home.cc"
        "apps/game/game_core.c"
        "apps/game/game_mic.c"
        "apps/game/game_sfx.c"
        "apps/game/app_game.cc"
        "apps/sound_jump/jump_core.c"
        "apps/sound_jump/jump_voice.c"
        "apps/sound_jump/jump_mic.c"
        "apps/sound_jump/app_sound_jump.cc"
        "apps/stopwatch/stopwatch_core.c"
        "apps/stopwatch/app_stopwatch.cc"
        "apps/pomodoro/pomodoro_core.c"
        "apps/pomodoro/app_pomodoro.cc"
        "apps/pomodoro/pomodoro_font_20.c"
        "apps/pomodoro/pomodoro_digits_48.c"
        "apps/muse/app_muse.cc"
        "${PASSPORT_MUSE_AVATAR}"
        "apps/game/tetris_core.c"
        "apps/game/app_tetris.cc"
    INCLUDE_DIRS
        "."
        "launcher"
        "apps"
        "apps/radio"
        "apps/voice"
        "apps/xiaozhi"
        "apps/home"
        "apps/game"
        "ui"
        "fonts"
    REQUIRES
        passport_muse
        bsp
        esp-wifi-connect
        esp_wifi
        nvs_flash
        esp_http_client
        esp_websocket_client
        json
        mbedtls
        esp_codec_dev
        esp_audio_codec
        esp_timer
        esp_pm
        spi_flash
        esp_driver_usb_serial_jtag
        esp_http_server
        spiffs
        lwip
)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=__wrap_opus_encoder_create,--undefined=__wrap_opus_encoder_destroy"
    "-Wl,--wrap=opus_encoder_create,--wrap=opus_encoder_destroy")

# Phone setup page: gzip at build time (mtime 0, reproducible); served as-is.
idf_build_get_property(python PYTHON)
set(phone_setup_html "${CMAKE_CURRENT_SOURCE_DIR}/apps/home/phone_setup.html")
set(phone_setup_gz "${CMAKE_CURRENT_BINARY_DIR}/phone_setup.html.gz")
add_custom_command(OUTPUT "${phone_setup_gz}"
    COMMAND "${python}" -c "import gzip,sys;open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))"
            "${phone_setup_html}" "${phone_setup_gz}"
    DEPENDS "${phone_setup_html}"
    VERBATIM)
add_custom_target(phone_setup_gz DEPENDS "${phone_setup_gz}")
add_dependencies(${COMPONENT_LIB} phone_setup_gz)
target_add_binary_data(${COMPONENT_LIB} "${phone_setup_gz}" BINARY)

if(PASSPORT_RADIO_HLS_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PASSPORT_RADIO_HLS_TEST=1)
endif()
