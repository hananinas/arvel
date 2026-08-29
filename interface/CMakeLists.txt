# Interface build integration for ESP-IDF
# This component builds the SvelteKit web interface and embeds it in the firmware

idf_component_register(SRCS "dummy.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES "esp_http_server")

# Get project directory
set(PROJECT_DIR ${CMAKE_SOURCE_DIR})
set(INTERFACE_DIR ${PROJECT_DIR}/interface)
set(OUTPUT_FILE ${PROJECT_DIR}/main/WWWData.h)
set(SOURCE_WWW_DIR ${INTERFACE_DIR}/src)
set(BUILD_DIR ${INTERFACE_DIR}/build)
set(BUILD_SCRIPT ${PROJECT_DIR}/scripts/build_interface_idf.py)

# Find the newest file in the interface source directory
file(GLOB_RECURSE INTERFACE_SOURCES "${SOURCE_WWW_DIR}/*")

# Custom command to build the web interface
add_custom_command(
    OUTPUT ${OUTPUT_FILE}
    COMMAND ${Python3_EXECUTABLE} ${BUILD_SCRIPT} ${PROJECT_DIR}
    DEPENDS ${INTERFACE_SOURCES}
    COMMENT "Building web interface and generating WWWData.h"
    VERBATIM
)

# Custom target to ensure the web interface is built
add_custom_target(build_web_interface ALL
    DEPENDS ${OUTPUT_FILE}
)

# Make the component depend on the web interface build
add_dependencies(${COMPONENT_LIB} build_web_interface)
