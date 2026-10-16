#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESK_NOTE_LIMIT 24
#define DESK_NOTE_TEXT_BYTES 256
#define DESK_FORECAST_HOURS 4

typedef enum { DESK_TIMER_IDLE, DESK_TIMER_RUNNING, DESK_TIMER_PAUSED, DESK_TIMER_DONE } desk_timer_status_t;
typedef enum { DESK_PHASE_TIMER, DESK_PHASE_FOCUS, DESK_PHASE_BREAK } desk_timer_phase_t;
typedef struct {
    uint32_t revision;
    desk_timer_status_t status;
    desk_timer_phase_t phase;
    bool pomodoro;
    uint8_t round, rounds;
    uint32_t remaining_seconds, duration_seconds;
    uint32_t notification_id;
    char label[129];
} desk_timer_snapshot_t;
typedef struct {
    uint32_t id;
    bool task, completed;
    char due_date[11]; /* YYYY-MM-DD in the user's local calendar, or empty. */
    char text[DESK_NOTE_TEXT_BYTES + 1];
} desk_note_t;
typedef struct {
    int64_t time; /* UTC Unix seconds; format in the returned location timezone. */
    double temperature_c;
    uint8_t precipitation_probability;
    int16_t weather_code;
} desk_forecast_hour_t;
typedef struct {
    uint32_t revision;
    int64_t fetched_at, observed_at;
    int32_t utc_offset_seconds; /* Use gmtime(time + offset) for station-local labels. */
    char location[97], timezone[65], condition[49];
    double latitude, longitude, temperature_c, feels_like_c, high_c, low_c, wind_kmh;
    int16_t weather_code;
    uint8_t forecast_count;
    desk_forecast_hour_t forecast[DESK_FORECAST_HOURS];
} desk_weather_snapshot_t;
/* The latest successful calculate or convert_units result. A conversion of the
 * value the preceding calculation produced keeps that calculation's expression. */
typedef struct {
    uint32_t revision;
    bool conversion;
    char expression[257]; /* calculate input, or empty for a directly given value */
    double input_value, value;
    char from[12], to[12]; /* convert_units tokens; empty for a calculation */
} desk_calc_snapshot_t;

/* Fast, thread-safe snapshots; no flash/network I/O. Keep larger output structs
 * in caller-owned persistent/heap storage rather than a small LVGL stack.
 * Timer notification_id increases on expiry; acknowledge that exact ID after
 * displaying a notification. Countdown state is monotonic and resets on boot.
 * Notes survive reboot. Weather returns false until a real fetch succeeds;
 * fetched_at identifies cached data, never an invented fresh observation.
 * Calculations reset on boot; the snapshot returns false until one succeeds. */
bool desk_buddy_timer_snapshot(desk_timer_snapshot_t *out);
void desk_buddy_timer_acknowledge(uint32_t notification_id);
bool desk_buddy_notes_ready(void);
size_t desk_buddy_note_count(void);
bool desk_buddy_note_get(size_t index, desk_note_t *out);
bool desk_buddy_weather_snapshot(desk_weather_snapshot_t *out);
bool desk_buddy_calc_snapshot(desk_calc_snapshot_t *out);

/* Async native-request completion. Invoked on the existing agent worker, NOT
 * the LVGL thread. Both pointers are borrowed only during this call; copy what
 * your UI needs and apply LVGL changes on its own thread. reply may be NULL on
 * allocation failure. Callback context must remain valid until completion.
 * Submit with ai_chat_submit_desk_tool() in ai_chat_bsp.h; no model request or
 * conversation-history mutation is needed for direct native utility controls. */
typedef void (*desk_buddy_reply_callback_t)(const char *tool, const cJSON *reply,
                                           bool success, void *context);

#ifdef __cplusplus
}
#endif
