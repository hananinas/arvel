idf_component_register(
  SRCS "ai_chat_bsp.cpp"
  
  INCLUDE_DIRS "."

  REQUIRES lvgl ui_bsp esp-tls esp_http_client esp_wifi esp_netif freertos cjson lwip esp_wifi_bsp)
