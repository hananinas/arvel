idf_component_register(
  SRCS "sdcard_bsp.c"
  REQUIRES esp_driver_sdmmc
  PRIV_REQUIRES fatfs sdmmc
  INCLUDE_DIRS "./")