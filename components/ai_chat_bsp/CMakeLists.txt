idf_component_register(
  SRCS "ai_chat_bsp.cpp" "buddy_settings.cpp" "device_tools.cpp" "desk_buddy.cpp" "desk_platform.cpp" "desk_tools.cpp"

  INCLUDE_DIRS "."

  REQUIRES lvgl ui_bsp lcd_bl_pwm_bsp esp_app_format esp-tls esp_http_client esp_wifi esp_netif esp_event freertos cjson lwip esp_wifi_bsp audio_bsp agent_runtime speech_stt speech_tts mbedtls nvs_flash esp_timer adc_bsp web_auth codex_auth pthread conversation_store nebius_sandbox)
