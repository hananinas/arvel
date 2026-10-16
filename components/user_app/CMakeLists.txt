idf_component_register(
  SRCS "user_app.cpp"
  PRIV_REQUIRES
  main
  ai_chat_bsp
  ble_scan_bsp
  esp_wifi_bsp
  esp_http_client
  cjson
  sdcard_bsp
  conversation_store
  lvgl
  ui_bsp
  button_bsp
  adc_bsp
  i2c_equipment
  lcd_bl_pwm_bsp
  audio_bsp
  espressif__esp_io_expander_tca9554
  web_server
  INCLUDE_DIRS "./")
