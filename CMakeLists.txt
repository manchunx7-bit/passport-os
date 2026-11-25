cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.2.0-rc.62")
option(PASSPORT_RADIO_HLS_TEST "Use curated HLS/global radio test stations" OFF)
option(PASSPORT_PUBLIC_RELEASE "Build with distribution-safe defaults and no personal configuration" OFF)
option(PASSPORT_MUSE_USE_JOLLYBOT "Build Muse with the official Jollybot artwork" ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# LVGL's default assert handler is `while(1);`, which freezes the screen with
# the LVGL lock held when an allocation fails on a nearly full heap. Panic
# instead so the device resets predictably and logs a backtrace. No `;` here:
# CMake would split the definition; the empty block ends the statement.
idf_build_set_property(COMPILE_DEFINITIONS "LV_ASSERT_HANDLER=while((__builtin_trap(),1)){}" APPEND)
project(passport-os)
include(tools/esp_tls_workaround.cmake)

# These are memory-safety requirements, not optional tuning. A generated root
# sdkconfig can silently retain old values after sdkconfig.defaults changes;
# that exact drift removed about 19 KB from the first internal-heap region and
# made the 22 KB Xiaozhi worker stack impossible to create on the real device.
foreach(required_option
        CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY
        CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUFFER
        CONFIG_MBEDTLS_DYNAMIC_BUFFER
        CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN)
    if(NOT ${required_option})
        message(FATAL_ERROR
            "${required_option} must be enabled for the ESP32-C3 memory budget. "
            "Build through ./build.sh or regenerate sdkconfig from sdkconfig.defaults.")
    endif()
endforeach()
if(NOT CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN EQUAL 8192)
    message(FATAL_ERROR
        "CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN must be 8192 for the ESP32-C3 audio/TLS budget.")
endif()

