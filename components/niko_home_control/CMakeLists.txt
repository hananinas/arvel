idf_component_register(
    SRCS "niko_home_control.cpp" "niko_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES agent_runtime
    PRIV_REQUIRES mqtt esp_timer json
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
