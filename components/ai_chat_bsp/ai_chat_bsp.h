#ifndef AI_CHAT_BSP_H
#define AI_CHAT_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "gui_guider.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

/**
 * @brief Initialize the AI chat component
 * 
 * @param ui Pointer to the LVGL UI structure
 * @param wifi_event_group WiFi event group handle for connection status
 */
void ai_chat_init(lv_ui *ui, EventGroupHandle_t wifi_event_group);

/**
 * @brief Set the OpenRouter API key
 * 
 * @param api_key The API key string (will be copied)
 */
void ai_chat_set_api_key(const char *api_key);

/**
 * @brief Set the OpenRouter model to use
 * 
 * @param model The model identifier (e.g., "openai/gpt-4o", "google/gemini-2.5-flash")
 * @note For audio input support, use models like "google/gemini-2.5-flash" that support audio
 */
void ai_chat_set_model(const char *model);

#ifdef __cplusplus
}
#endif

#endif // AI_CHAT_BSP_H
