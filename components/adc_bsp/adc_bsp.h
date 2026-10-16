#ifndef ADC_BSP_H
#define ADC_BSP_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Battery sense: BAT_ADC on GPIO4 (ADC1 channel 3) through a 1:3 divider. */
typedef struct {
    int millivolts;     /* averaged cell voltage */
    int percent;        /* 0..100, estimated from a Li-ion resting-voltage curve */
    bool present;       /* false when the reading is too low for a connected cell */
    bool calibrated;    /* true when eFuse ADC calibration was available */
} battery_reading_t;

/* Safe to call more than once; returns an error instead of aborting. */
esp_err_t adc_bsp_init(void);
/* Thread-safe. Charging raises the voltage, so the percentage is an estimate. */
esp_err_t battery_read(battery_reading_t *reading);

#ifdef __cplusplus
}
#endif

#endif
