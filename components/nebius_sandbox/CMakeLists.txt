idf_component_register(
  SRCS "nebius_sandbox.cpp" "nebius_platform.cpp"
  INCLUDE_DIRS "include"
  REQUIRES agent_runtime speech_stt esp_http_client esp_timer freertos cjson esp-tls pthread heap log)
