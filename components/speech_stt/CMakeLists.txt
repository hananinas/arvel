idf_component_register(
    SRCS "speech_stt.cpp" "speech_stt_protocol.cpp" "bounded_https.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_client tcp_transport esp_timer lwip freertos mbedtls esp_hw_support heap log cjson)
