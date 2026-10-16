idf_component_register(
    SRCS "conversation_store.cpp" "checked_vfs.cpp" "history_tool.cpp" "vendor/sqlite3.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "vendor"
    REQUIRES agent_runtime
    PRIV_REQUIRES sdcard_bsp fatfs esp_timer mbedtls cjson pthread)
# Keep configuration identical in the owning service, checked VFS and amalgamation.
target_compile_options(${COMPONENT_LIB} PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/sqlite_config.h")
set_source_files_properties("vendor/sqlite3.c" PROPERTIES
    COMPILE_OPTIONS "-Os;-Wno-unused-function;-Wno-unused-variable"
    COMPILE_DEFINITIONS "ARVEL_SQLITE_AMALGAMATION=1")
