file(GLOB ARVEL_FONTS "${CMAKE_CURRENT_LIST_DIR}/fonts/*.c")
idf_component_register(
  SRCS "buddy_ui.c" "arvel_art.c" ${ARVEL_FONTS}
  REQUIRES lvgl
  INCLUDE_DIRS "." "custom")
