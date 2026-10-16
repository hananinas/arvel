idf_component_register(
    SRCS "codex_auth.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES speech_stt esp_http_client esp_timer freertos nvs_flash heap log cjson mbedtls)
