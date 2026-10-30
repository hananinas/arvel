idf_component_register(SRCS "web_server.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server nvs_flash esp_wifi esp_wifi_bsp main ai_chat_bsp json web_auth codex_auth speech_tts conversation_store)
