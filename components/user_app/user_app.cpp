#include <stdio.h>
#include "user_app.h"
#include "lvgl.h"
#include "gui_guider.h"
#include "esp_log.h"
#include "esp_system.h"
#include "user_config.h"
#include "lcd_bl_pwm_bsp.h"
#include "esp_wifi_bsp.h"
#include "esp_io_expander_tca9554.h"
#include "user_audio_bsp.h"
#include "freertos/event_groups.h"
#include "ai_chat_bsp.h"
#include "web_server.h"

static const char *TAG = "user_app";
static httpd_handle_t web_server = NULL;

lv_ui src_ui;
esp_io_expander_handle_t io_expander = NULL;

/*静态函数*/
static void tca9554_init(void);

static void tca9554_init(void)
{
	i2c_master_bus_handle_t tca9554_i2c_bus_ = NULL;
	ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &tca9554_i2c_bus_));
	esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
	esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);
	esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, 1);
}

void user_app_init(void)
{
	ESP_LOGI(TAG, "Initializing AI Audio Chat application");

	// Initialize UI
	setup_ui(&src_ui);
	
	// Show AI chat screen and hide demo screens
	lv_obj_clear_flag(src_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);  // Show AI chat
	
	
	// Initialize WiFi
	espwifi_init();
	
	// Initialize AI Chat with OpenRouter
	ai_chat_init(&src_ui, wifi_even_);
	ai_chat_set_api_key("");
	ai_chat_set_model("google/gemini-2.5-flash-preview-09-2025");
	
	// Initialize LCD backlight PWM
	lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
	
	// Initialize I/O expander for power management
	tca9554_init();
	
	// Initialize audio codec
	user_audio_bsp_init();
	audio_play_init();

	// Start web server (after WiFi is initialized)
	ESP_LOGI(TAG, "Starting web server...");
	web_server = web_server_start();
	if (web_server) {
		ESP_LOGI(TAG, "Web server started successfully");
		ESP_LOGI(TAG, "Access web interface at: http://<device-ip>/");
	} else {
		ESP_LOGE(TAG, "Failed to start web server");
	}

	ESP_LOGI(TAG, "AI Audio Chat application ready");
}
