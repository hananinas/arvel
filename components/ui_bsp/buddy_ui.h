#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUDDY_IDLE,
    BUDDY_CLOCK_SYNC,
    BUDDY_RECORDING,
    BUDDY_PREVIEW,
    BUDDY_TRANSCRIBING,
    BUDDY_THINKING,
    BUDDY_SPEAKING,
    BUDDY_ERROR,
    BUDDY_LISTENING,
    BUDDY_MUTED,
    BUDDY_VOICE_CONNECTING
} buddy_ui_state_t;

/* Preferences shown in the settings panel; enumerations index the panel options
 * (voice: Warm/Bright/Soft, sleep: 1 min/5 min/Never, persona: Calm/Chatty/Focused). */
typedef struct {
    uint8_t voice_style;
    uint8_t speech_rate;   /* tenths, 5..20 */
    bool captions;
    uint8_t brightness;    /* percent, 10..100 */
    bool dim_at_night;
    uint8_t screen_sleep;
    uint8_t persona;
    uint8_t volume;        /* percent, 0..100 */
    bool read_aloud;
} buddy_ui_prefs_t;

typedef enum {
    BUDDY_UI_SAVED,
    BUDDY_UI_SAVE_BUSY,    /* retried automatically once Arvel is idle */
    BUDDY_UI_SAVE_FAILED
} buddy_ui_save_result_t;

typedef struct {
    /* Mic: start live voice chat or mute/unmute; local mode records/sends. */
    bool (*voice)(void *context);
    /* Live preview of preferences (backlight, volume, voice) without saving. */
    void (*apply_prefs)(const buddy_ui_prefs_t *prefs, void *context);
    /* Persists preferences and the scene id. */
    buddy_ui_save_result_t (*save)(const buddy_ui_prefs_t *prefs, const char *scene, void *context);
    void *context;
    /* Ends live voice or cancels pending transcription/model I/O. */
    bool (*voice_stop)(void *context);
    /* Physical display actions only; the password must never leave this UI. */
    bool (*wifi_setup_start)(char *password, size_t capacity, void *context);
    void (*wifi_setup_stop)(void *context);
    uint32_t (*wifi_setup_remaining)(void *context);
    /* Invoked only after a separate explicit touchscreen confirmation.
     * true means request accepted, not observed restart/reset completion. */
    bool (*on_restart)(void *context);
    bool (*on_factory_reset)(void *context);
    /* Physical web-owner approval only; six digits, never a Wi-Fi password.
     * remaining returns zero on expiry, cancellation or successful setup. */
    bool (*web_login_start)(char *code, size_t capacity, void *context);
    void (*web_login_stop)(void *context);
    uint32_t (*web_login_remaining)(void *context);
    /* Tool pages' controls (Pause, +5 min, a to-do's checkbox, a unit, ...) as a
     * native desk-tool request: tool name and a JSON object of arguments. true
     * means the request was accepted; results arrive through the setters below. */
    bool (*desk_tool)(const char *tool, const char *arguments_json, void *context);
    /* The timer page has shown this expiry; the backend stops announcing it. */
    void (*timer_seen)(uint32_t notification_id, void *context);
} buddy_ui_callbacks_t;

/* Tool-call icon kinds, in the design's ToolIcon order. */
typedef enum {
    BUDDY_TOOL_SEARCH,
    BUDDY_TOOL_READ,
    BUDDY_TOOL_RUN,
    BUDDY_TOOL_EDIT,
    BUDDY_TOOL_MCP,
    BUDDY_TOOL_GIT,
    BUDDY_TOOL_APPROVE,
    BUDDY_TOOL_HARNESS,
    BUDDY_TOOL_TIMER,
    BUDDY_TOOL_NOTES,
    BUDDY_TOOL_CALC,
    BUDDY_TOOL_WEATHER
} buddy_ui_tool_kind_t;

typedef enum {
    BUDDY_TOOL_RUNNING,
    BUDDY_TOOL_SUCCEEDED,
    BUDDY_TOOL_FAILED,
    BUDDY_TOOL_UNKNOWN,
    BUDDY_TOOL_CANCELLED,
    BUDDY_TOOL_PARTIAL
} buddy_ui_tool_state_t;

/* One tool call: a short verb ("Read") and its target ("ci.yml", or ""). */
typedef struct {
    buddy_ui_tool_kind_t kind;
    const char *verb;
    const char *target;
    buddy_ui_tool_state_t state;
    const char *id;        /* Actual provider call ID, never a fabricated sequence. */
    const char *error;     /* Bounded public error, or "". */
} buddy_ui_tool_t;

typedef struct {
    uint32_t id;
    bool assistant;
    const char *text;
    const char *time;      /* local "HH:MM", or "" when unknown */
    const buddy_ui_tool_t *tools;  /* calls made for this reply (assistant only) */
    size_t tool_count;
} buddy_ui_message_t;

typedef enum {
    BUDDY_UI_TIMER_IDLE,
    BUDDY_UI_TIMER_RUNNING,
    BUDDY_UI_TIMER_PAUSED,
    BUDDY_UI_TIMER_DONE
} buddy_ui_timer_status_t;

/* The on-device timer; a Pomodoro counts rounds and alternates focus and break. */
typedef struct {
    buddy_ui_timer_status_t status;
    bool pomodoro;
    bool on_break;
    uint8_t round, rounds;
    uint32_t remaining_seconds, duration_seconds;
    uint32_t notification_id;  /* nonzero: an expiry not yet shown */
    const char *label;
} buddy_ui_timer_t;

/* A saved note or to-do. due is a short local label ("Today", "Fri", "14 Oct")
 * or ""; urgent marks today's and overdue tasks. */
typedef struct {
    uint32_t id;
    bool task;
    bool completed;
    bool urgent;
    const char *text;
    const char *due;
} buddy_ui_note_t;

/* The latest on-device calculation; a conversion also names its units (the
 * convert_units tokens, e.g. "kg" and "lb"). expression is what produced the
 * input value ("3 × 1.25"), or "" when the value was given directly. */
typedef struct {
    bool conversion;
    const char *expression;
    double input;
    const char *from, *to;
    double value;
} buddy_ui_calc_t;

typedef struct {
    const char *time;          /* station-local "HH:MM" */
    int16_t weather_code;      /* WMO code */
    int temperature_c;
    uint8_t precipitation_probability;
} buddy_ui_forecast_t;

/* The last real weather fetch, in whole degrees Celsius. */
typedef struct {
    const char *location;
    const char *observed;      /* station-local "HH:MM" */
    const char *condition;
    int16_t weather_code;
    int temperature_c, feels_like_c, high_c, low_c;
    const buddy_ui_forecast_t *forecast;
    size_t forecast_count;
} buddy_ui_weather_t;

typedef struct {
    const char *buddy_name;
    const char *user_name;
    const char *model;
    const char *firmware;
    bool chat_configured;
    bool voice_configured;
    const char *storage_state;    /* Actual storage status, never a persistence promise. */
    const char *conversation_id;
    const char *native_readiness; /* Authenticated health state; audio remains unexercised. */
} buddy_ui_profile_t;

/* All functions run on the LVGL thread or while holding its existing lock.
 * Input strings are copied; caller-owned pointers are not retained. */
void buddy_ui_init(const buddy_ui_callbacks_t *callbacks);
void buddy_ui_set_profile(const buddy_ui_profile_t *profile);
void buddy_ui_set_prefs(const buddy_ui_prefs_t *prefs, const char *scene);
void buddy_ui_set_status(buddy_ui_state_t state, const char *detail, bool busy);
/* Live microphone state and real incremental caption; NULL transcript clears it. */
void buddy_ui_set_voice(bool available, bool active, bool muted, bool connecting,
                        const char *role, const char *transcript);
void buddy_ui_set_network(bool connected, const char *ssid, const char *ip);
/* Local wall-clock time ("HH:MM") and hour (0..23), or ready=false before sync. */
void buddy_ui_set_clock(bool ready, const char *local_time, int hour);
void buddy_ui_set_messages(const buddy_ui_message_t *messages, size_t count);
/* Tool calls of the reply in progress, oldest first; shown while thinking. */
void buddy_ui_set_tools(const buddy_ui_tool_t *tools, size_t count);
/* Words per minute of the current speech, used to pace live captions. */
void buddy_ui_set_speech_pace(int words_per_minute);
/* While asleep the screen shows nothing new; the first touch only wakes it. */
void buddy_ui_set_asleep(bool asleep);
/* Tool pages. NULL means unavailable (no timer, notes storage unreadable,
 * nothing calculated or fetched yet). A timer expiry opens its page. */
void buddy_ui_set_timer(const buddy_ui_timer_t *timer);
void buddy_ui_set_notes(const buddy_ui_note_t *notes, size_t count, bool ready);
void buddy_ui_set_calc(const buddy_ui_calc_t *calc);
void buddy_ui_set_weather(const buddy_ui_weather_t *weather);

#ifdef __cplusplus
}
#endif
