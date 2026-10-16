#include <stdio.h>
#include "user_app.h"
#include "lvgl.h"
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
#include "driver/gpio.h"
#include "adc_bsp.h"
#include "button_bsp.h"
#include "freertos/task.h"
#include "conversation_store.hpp"
#include <time.h>

static const char *TAG = "user_app";
static httpd_handle_t web_server = NULL;

static esp_io_expander_handle_t io_expander = NULL;

void user_app_power_init(void)
{
	i2c_master_bus_handle_t tca9554_i2c_bus_ = NULL;
	ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &tca9554_i2c_bus_));
	ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));

	// Latch battery power before LCD setup, Wi-Fi, or other lengthy initialization.
	ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, IO_EXPANDER_OUTPUT));
	ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 1));
	// Preserve the speaker amplifier enable on both board revisions.
	ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, IO_EXPANDER_OUTPUT));
	ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, 1));

#if CONFIG_UNRAVEL_BOARD_V2
	ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_BL_EN | EXAMPLE_EXIO_PIN_LCD_RST, IO_EXPANDER_OUTPUT));
	ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 0));
	ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
#endif
}

esp_err_t user_app_lcd_reset_set_level(uint32_t level)
{
#if CONFIG_UNRAVEL_BOARD_V2
	if (io_expander == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	return esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, level);
#else
	return gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST, level);
#endif
}

void user_app_power_off(void)
{
	ai_chat_voice_stop(AI_CHAT_NATIVE);
	setUpduty(0xff); // Inverted PWM duty: 0xff is backlight off.
	vTaskDelay(pdMS_TO_TICKS(120));
	if (io_expander != NULL) {
		ESP_ERROR_CHECK_WITHOUT_ABORT(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 0));
	}
}

/* Top-edge physical buttons (button_bsp: BOOT = GPIO0, PWR = GPIO16).
 * PWR click toggles the screen, PWR long-press powers off, BOOT click (or
 * long-press start) cycles screen brightness. Events arrive via the vendor
 * multi-button event groups; handling runs here, never in the 5 ms timer. */
static void button_task(void *arg)
{
	(void)arg;
	for (;;) {
		EventBits_t pwr = xEventGroupWaitBits(pwr_groups, set_bit_button(0) | set_bit_button(1),
		                                      pdTRUE, pdFALSE, pdMS_TO_TICKS(50));
		if (pwr & set_bit_button(1)) {
			user_app_power_off();
		} else if (pwr & set_bit_button(0)) {
			ESP_LOGI(TAG, "PWR click: screen %s", ai_chat_toggle_screen() ? "off" : "on");
		}
		EventBits_t boot = xEventGroupWaitBits(boot_groups, set_bit_button(0) | set_bit_button(2),
		                                       pdTRUE, pdFALSE, 0);
		if (boot & (set_bit_button(0) | set_bit_button(2))) {
			ai_chat_cycle_brightness();
		}
	}
}

void user_app_init(void)
{
	ESP_LOGI(TAG, "Initializing Unravel Buddy");
	espwifi_init();
	// Battery sense is optional: without it the battery tool reports unavailable.
	esp_err_t battery_err = adc_bsp_init();
	if (battery_err != ESP_OK) ESP_LOGW(TAG, "Battery sense unavailable: %s", esp_err_to_name(battery_err));
	user_audio_bsp_init();
	audio_play_init();
	// Mount/SQLite work belongs to the storage task, never this LVGL caller.
	conversation_store::Config archive;
	archive.trusted_utc_now_ms = []() -> int64_t {
		const time_t now = time(nullptr);
		return now >= 1704067200 ? static_cast<int64_t>(now) * 1000 : 0;
	};
	if (conversation_store::start_async(archive) != conversation_store::Error::none)
		ESP_LOGW(TAG, "Local history storage task is unavailable");
	ai_chat_init(wifi_even_);
	
	// Initialize LCD backlight PWM
	lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
	
#if CONFIG_UNRAVEL_BOARD_V2
	ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 1));
#endif
	

	// Start web server (after WiFi is initialized)
	ESP_LOGI(TAG, "Starting web server...");
	web_server = web_server_start();
	if (web_server) {
		ESP_LOGI(TAG, "Web server started successfully");
		ESP_LOGI(TAG, "Access web interface at: http://<device-ip>/");
	} else {
		ESP_LOGE(TAG, "Failed to start web server");
	}

	// Top-edge BOOT/PWR buttons (GPIO0/GPIO16); see button_task above.
	button_Init();
	if (xTaskCreate(button_task, "top_buttons", 2048, NULL, 3, NULL) != pdPASS) {
		ESP_LOGW(TAG, "Top buttons unavailable: button task not created");
	}

	ESP_LOGI(TAG, "Unravel Buddy application ready");
}
