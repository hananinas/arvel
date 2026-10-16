#ifndef USER_APP_H
#define USER_APP_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call immediately after I2C initialization to latch battery power.
void user_app_power_init(void);
esp_err_t user_app_lcd_reset_set_level(uint32_t level);

void user_app_init(void);
// Drops the battery power latch (TCA9554 SYS_EN) after stopping voice and
// blanking the backlight. On USB power the board may stay powered; the latch
// drop is still the hardware power-off request.
void user_app_power_off(void);

#ifdef __cplusplus
}
#endif

#endif