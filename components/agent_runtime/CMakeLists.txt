idf_component_register(
    SRCS "agent_runtime.cpp"
    INCLUDE_DIRS "include"
    REQUIRES cjson
    PRIV_REQUIRES pthread)

if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "The embedded Rust agent currently targets ESP32-S3")
endif()
find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(RUST_DIR "${CMAKE_CURRENT_LIST_DIR}/rust")
set(RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(RUST_LIBRARY "${RUST_TARGET_DIR}/xtensa-esp32s3-espidf/release/libunravel_agent_esp32.a")
file(GLOB_RECURSE RUST_SOURCES "${RUST_DIR}/src/*.rs"
    "${RUST_DIR}/vendor/*.rs" "${RUST_DIR}/vendor/*.toml")
list(APPEND RUST_SOURCES "${RUST_DIR}/Cargo.toml")
add_custom_command(
    OUTPUT "${RUST_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E env
        "RUSTFLAGS=--cfg espidf_time64 -C panic=abort"
        "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
        ${CARGO_EXECUTABLE} +esp build --release --locked
        --target xtensa-esp32s3-espidf -Zbuild-std=std,panic_abort
    WORKING_DIRECTORY "${RUST_DIR}"
    DEPENDS ${RUST_SOURCES} "${RUST_DIR}/Cargo.lock"
    COMMENT "Building on-device Rust agent runtime"
    VERBATIM)
add_custom_target(build_rust_agent DEPENDS "${RUST_LIBRARY}")
add_dependencies(${COMPONENT_LIB} build_rust_agent)
target_link_libraries(${COMPONENT_LIB} INTERFACE "${RUST_LIBRARY}")
