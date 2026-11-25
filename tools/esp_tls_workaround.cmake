# Compile a patched copy, never edit the installed SDK or third-party licenses.
idf_component_get_property(passport_tls_lib esp-tls COMPONENT_LIB)
idf_component_get_property(passport_tls_dir esp-tls COMPONENT_DIR)
idf_build_get_property(passport_python PYTHON)
set(passport_tls_original "${passport_tls_dir}/esp_tls.c")
set(passport_tls_patched "${CMAKE_BINARY_DIR}/passport-patches/esp_tls.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${passport_tls_original}" "${CMAKE_CURRENT_LIST_DIR}/patch_esp_tls.py")
execute_process(COMMAND "${passport_python}"
    "${CMAKE_CURRENT_LIST_DIR}/patch_esp_tls.py"
    "${passport_tls_original}" "${passport_tls_patched}"
    RESULT_VARIABLE passport_tls_patch_result)
if(NOT passport_tls_patch_result EQUAL 0)
    message(FATAL_ERROR "ESP-TLS async-connect workaround needs review for this SDK")
endif()
get_target_property(passport_tls_sources ${passport_tls_lib} SOURCES)
list(FIND passport_tls_sources "${passport_tls_original}" passport_tls_source_index)
if(passport_tls_source_index EQUAL -1)
    message(FATAL_ERROR "Cannot locate ESP-TLS source for async-connect workaround")
endif()
list(REMOVE_AT passport_tls_sources ${passport_tls_source_index})
list(APPEND passport_tls_sources "${passport_tls_patched}")
set_property(TARGET ${passport_tls_lib} PROPERTY SOURCES "${passport_tls_sources}")
