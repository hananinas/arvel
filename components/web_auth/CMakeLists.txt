idf_component_register(SRCS "web_auth.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_common
                       PRIV_REQUIRES nvs_flash mbedtls esp_timer esp_hw_support freertos)
