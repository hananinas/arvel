#include "buddy_ui.h"
#include "arvel_art.h"
#include "fonts/arvel_fonts.h"
#include "lvgl.h"
#include "extra/libs/png/lodepng.h"

#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* Arvel screen, implemented pixel-for-pixel from the "Arvel Buddy — ESP32
 * Display" design: 640x172, scene backdrop, Arvel on the left, a speech bubble
 * and a rail of three round buttons. Coordinates and colours below are the
 * design's own values. */

#define SCREEN_W 640
#define SCREEN_H 172
#define MAX_MESSAGES 8
#define MAX_LAYERS 12
#define MAX_TRACKS 112
#define SCENES_PER_PAGE 6
#define TAB_COUNT 4
#define ROWS_PER_TAB 4
#define REPLY_LINGER_MS 6000
#define SILENT_REPLY_MS 20000
#define PANEL_TIMEOUT_MS 45000
#define SAVE_DELAY_MS 700
#define SAVE_RETRY_MS 2000
#define FRAME_MS 33
#define TEXT_MAX 8193
#define THINK_ROWS 2          /* latest tool calls listed while thinking */
#define CHAT_CHIPS 3          /* tool chips kept per reply in the chat log */
#define SPIN_MS 900
#define DESK_HOURS 4          /* hourly forecasts on the weather page */
#define NOTE_ROWS 24          /* the backend keeps at most this many notes */
#define TIMER_DOTS 12         /* most Pomodoro rounds */
#define PAGE_NOTICE_MS 4000
#define SUGGESTIONS 4

#define F10R (&arvel_font_10_400)
#define F10S (&arvel_font_10_600)
#define F11R (&arvel_font_11_400)
#define F11M (&arvel_font_11_500)
#define F11B (&arvel_font_11_700)
#define F11T (&arvel_font_11_650)
#define F12R (&arvel_font_12_400)
#define F12M (&arvel_font_12_500)
#define F12S (&arvel_font_12_600)
#define F12T (&arvel_font_12_650)
#define F12B (&arvel_font_12_700)
#define F12I (&arvel_font_12_400i)
#define F13R (&arvel_font_13_400)
#define F13M (&arvel_font_13_500)
#define F13D (&arvel_font_13_550)
#define F13S (&arvel_font_13_650)
#define F14R (&arvel_font_14_400)
#define F14T (&arvel_font_14_650)
#define F15S (&arvel_font_15_600)
#define F15B (&arvel_font_15_650)
#define F17D (&arvel_font_17_550)
#define F18D (&arvel_font_18_550)
#define F18S (&arvel_font_18_600)
#define F19T (&arvel_font_19_650)
#define F22B (&arvel_font_22_700)
#define F34B (&arvel_font_34_700)
#define F17B (&arvel_font_17_650)
#define F21B (&arvel_font_21_650)
#define M11 (&arvel_mono_11)
#define M12 (&arvel_mono_12)

#define MIC_RED 0xA65B48
#define READY_GREEN 0x628B7E
#define THINK_AMBER 0xD9A63A
#define SHADOW_INK 0x1E2824
#define TIMER_RED 0xE5674F
#define BELL_RED 0xC0503F
#define DONE_HALO 0xFCE3DE
#define NOTE_BAR 0xE2B65C

typedef enum {
    MODE_IDLE, MODE_LISTENING, MODE_THINKING, MODE_SPEAKING, MODE_CHAT, MODE_SCENES, MODE_SETTINGS,
    /* Tool pages, shown in the bubble with their own header. */
    MODE_TIMER, MODE_TIMER_DONE, MODE_NOTES, MODE_CALC, MODE_WEATHER
} ui_mode_t;

typedef enum {
    G_SCENE, G_BIRD, G_LISTEN, G_THINK, G_SPEAK, G_BELL, G_COUNT
} track_group_t;

typedef enum {
    P_OPA, P_X, P_Y, P_H, P_TRANSLATE_Y, P_RADIUS, P_ANGLE
} track_prop_t;

/* One SMIL-style keyframe animation applied to an object property. */
typedef struct {
    lv_obj_t *obj;
    const int16_t *values;
    const uint16_t *key_times;  /* permille, or NULL for evenly spaced values */
    uint16_t period;
    uint16_t begin;
    int16_t base_x, base_y;     /* offsets for P_X/P_Y; centre for P_RADIUS */
    int16_t last;
    uint8_t count;
    uint8_t prop;
    uint8_t group;
    uint8_t divisor;            /* values are in 1/divisor pixels */
} track_t;

typedef enum {
    R_VOICE, R_SPEED, R_HEALTH, R_CAPTIONS,
    R_BRIGHT, R_SCENE, R_DIM, R_SLEEP,
    R_PERSONA, R_MODEL, R_ALOUD, R_MEMORY,
    R_VOLUME, R_WIFI, R_WEB_LOGIN, R_ACTIONS
} row_id_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *label;
    lv_obj_t *toggle;
    lv_obj_t *knob;
    lv_obj_t *stepper;
    lv_obj_t *step_value;
    lv_obj_t *seg;
    lv_obj_t *seg_opts[3];
    lv_obj_t *seg_labels[3];
    lv_obj_t *value;
    lv_obj_t *value_label;
} row_slot_t;

typedef struct {
    lv_color_t ink, sub, faint, accent, bubble, rail, rail_ink, chip, chip_ink;
    lv_color_t user_bg, user_ink, rule, track_off, seg_on, ring_gap, active_ink, tool_chip;
    lv_opa_t bubble_opa, rail_opa, chip_opa, user_opa, rule_opa, tool_chip_opa;
    /* tool pages */
    lv_color_t ring_track, dot_off, note_bg, urgent_bg, urgent_ink, box_border;
    lv_opa_t ring_track_opa, dot_off_opa, note_bg_opa, urgent_opa;
} palette_t;

/* A tool call as shown: icon kind, verb and monospace target. */
typedef struct {
    uint8_t kind;
    buddy_ui_tool_state_t state;
    char verb[16];
    char target[48];
    char id[96];
    char error[160];
} tool_slot_t;

/* Thinking-list row or chat chip widgets for one tool call. */
typedef struct {
    lv_obj_t *row, *icon, *verb, *target, *spinner, *check;
} tool_view_t;

/* A list row of the to-do page: a task with its checkbox, or a note. */
typedef struct {
    lv_obj_t *task, *box, *check, *label, *tag;
    lv_obj_t *note, *note_label;
} note_view_t;

typedef struct {
    uint32_t id;
    bool task, completed, urgent;
    char due[16];
    char text[257];
} note_slot_t;

/* A convert_units token, the name the page shows, and its linear scale to SI. */
typedef struct {
    const char *token, *name;
    uint8_t dimension;
    double scale, offset;
} unit_t;

/* Unit choices in the order the calculator offers them, by dimension (the backend's table). */
static const unit_t UNITS[] = {
    {"lb", "lb", 0, 0.45359237, 0}, {"oz", "oz", 0, 0.028349523125, 0}, {"g", "g", 0, 0.001, 0}, {"kg", "kg", 0, 1, 0},
    {"km", "km", 1, 1000, 0}, {"m", "m", 1, 1, 0}, {"cm", "cm", 1, 0.01, 0}, {"mm", "mm", 1, 0.001, 0},
    {"mi", "mi", 1, 1609.344, 0}, {"yd", "yd", 1, 0.9144, 0}, {"ft", "ft", 1, 0.3048, 0}, {"in", "in", 1, 0.0254, 0},
    {"l", "l", 2, 1, 0}, {"ml", "ml", 2, 0.001, 0}, {"us_gal", "gal", 2, 3.785411784, 0}, {"us_fl_oz", "fl oz", 2, 0.0295735295625, 0},
    {"min", "min", 3, 60, 0}, {"h", "h", 3, 3600, 0}, {"s", "s", 3, 1, 0}, {"day", "day", 3, 86400, 0},
    {"c", "°C", 4, 1, 273.15}, {"f", "°F", 4, 5.0 / 9.0, 273.15 - 32.0 * 5.0 / 9.0}, {"k", "K", 4, 1, 0},
    {"km_h", "km/h", 5, 1.0 / 3.6, 0}, {"mph", "mph", 5, 0.44704, 0}, {"m_s", "m/s", 5, 1, 0},
};
#define UNIT_COUNT (sizeof(UNITS) / sizeof(UNITS[0]))
#define TEMPERATURE 4

/* The idle bubble's shortcuts to the tool pages. */
static const struct {
    uint8_t kind;
    const char *label;
} SUGGEST[SUGGESTIONS] = {
    {BUDDY_TOOL_TIMER, "Focus 25m"}, {BUDDY_TOOL_NOTES, "To-dos"},
    {BUDDY_TOOL_CALC, "Convert"}, {BUDDY_TOOL_WEATHER, "Weather"},
};

static const char *const VOICE_OPTS[3] = {"Warm", "Bright", "Soft"};
static const char *const SLEEP_OPTS[3] = {"1 min", "5 min", "Never"};
static const char *const PERSONA_OPTS[3] = {"Calm", "Chatty", "Focused"};
static const char *const TAB_NAMES[TAB_COUNT] = {"Voice", "Display", NULL, "Device"};
static const row_id_t TAB_ROWS[TAB_COUNT][ROWS_PER_TAB] = {
    {R_VOICE, R_SPEED, R_HEALTH, R_CAPTIONS},
    {R_BRIGHT, R_SCENE, R_DIM, R_SLEEP},
    {R_PERSONA, R_MODEL, R_ALOUD, R_MEMORY},
    {R_VOLUME, R_WIFI, R_WEB_LOGIN, R_ACTIONS},
};

/* Design animations, as SMIL keyframes. */
static const int16_t BOB[] = {0, -28, 0};                  /* 1.3 units of 80 at 170px, in tenths */
static const int16_t BLINK[] = {0, 0, 255, 0, 0};
static const uint16_t BLINK_TIMES[] = {0, 900, 930, 960, 1000};
static const int16_t RIPPLE_R[] = {34, 72};
static const int16_t RIPPLE_OPA[] = {140, 0};
static const int16_t DOT_OPA[] = {64, 255, 64};
static const int16_t WAVE_OPA[] = {77, 255, 77};
static const int16_t BELL_SWING[] = {0, 160, -140, 100, -60, 0, 0};   /* tenths of a degree */
static const uint16_t BELL_TIMES[] = {0, 100, 200, 300, 400, 500, 1000};
static const struct {
    int16_t x;
    uint16_t period;
    uint8_t count;
    int16_t h[4], y[4];
} BARS[10] = {
    {0, 900, 3, {6, 18, 6}, {9, 3, 9}},          {9, 1100, 4, {12, 22, 8, 12}, {6, 1, 8, 6}},
    {18, 800, 4, {16, 6, 20, 16}, {4, 9, 2, 4}}, {27, 1000, 3, {8, 20, 8}, {8, 2, 8}},
    {36, 700, 3, {20, 10, 20}, {2, 7, 2}},       {45, 1200, 3, {10, 22, 10}, {7, 1, 7}},
    {54, 850, 3, {14, 6, 14}, {5, 9, 5}},        {63, 950, 3, {6, 16, 6}, {9, 4, 9}},
    {72, 1050, 3, {12, 20, 12}, {6, 2, 6}},      {81, 750, 3, {6, 12, 6}, {9, 6, 9}},
};

/* Events and setters run on the caller's LVGL thread/lock. */
static struct {
    buddy_ui_callbacks_t callbacks;
    lv_obj_t *screen;
    palette_t pal;
    bool dark;

    /* scene */
    lv_obj_t *scene_box;
    lv_obj_t *scene_objs[MAX_LAYERS];
    lv_img_dsc_t *scene_imgs[MAX_LAYERS];
    size_t scene_layer_count;
    size_t scene;
    bool scene_loaded;
    lv_img_dsc_t *thumbs[ARVEL_SCENE_COUNT];

    /* character */
    lv_obj_t *ripple_box;
    lv_obj_t *ripples[2];
    lv_obj_t *bird_box;
    lv_img_dsc_t *bird_img;
    lv_img_dsc_t *blink_img;

    /* pill */
    lv_obj_t *pill;
    lv_obj_t *clock;
    lv_obj_t *wifi;

    /* bubble */
    lv_obj_t *bubble;
    lv_obj_t *tail;
    lv_obj_t *status_row;
    lv_obj_t *status_dot;
    lv_obj_t *status_label;
    lv_obj_t *model_label;
    lv_obj_t *idle_box, *idle_title, *idle_sub, *suggest_row, *suggest[SUGGESTIONS];
    lv_obj_t *listen_box, *listen_words;
    lv_obj_t *think_box, *think_title, *think_dots[3], *think_quote, *think_tools, *think_spacer, *think_footer;
    tool_view_t think_rows[THINK_ROWS];
    lv_obj_t *speak_box, *speak_scroll, *speak_faint, *speak_ink, *speak_icons, *speak_hint;
    lv_obj_t *live_box, *live_role, *live_scroll, *live_words, *live_empty, *live_hint;
    lv_obj_t *chat_box, *chat_dot, *chat_time, *chat_dots, *chat_older, *chat_newer;
    lv_obj_t *chat_content, *chat_q_row, *chat_q, *chat_q_label, *chat_q_corner, *chat_tools, *chat_a;
    tool_view_t chat_chips[CHAT_CHIPS];
    lv_img_dsc_t *tool_imgs[ARVEL_TOOL_COUNT][2];
    lv_obj_t *page_dots[MAX_MESSAGES];

    /* tool pages: shared header, then one box per page */
    lv_obj_t *page_box, *page_icon, *page_title, *page_meta, *page_close;
    lv_obj_t *timer_box, *timer_ring, *timer_time, *timer_sub, *timer_label, *timer_rounds;
    lv_obj_t *timer_dots[TIMER_DOTS], *timer_buttons[3], *timer_button_labels[3];
    lv_obj_t *done_box, *done_halo, *done_title, *done_sub, *done_buttons[2], *done_button_labels[2];
    lv_img_dsc_t *bell_img;
    track_t bell_track;
    lv_obj_t *notes_box, *notes_empty;
    note_view_t note_views[NOTE_ROWS];
    size_t note_view_count;
    lv_obj_t *calc_box, *calc_expr, *calc_value_row, *calc_value, *calc_unit, *calc_bottom;
    lv_obj_t *calc_seg, *calc_opts[3], *calc_opt_labels[3], *calc_rate, *calc_empty;
    lv_obj_t *weather_box, *weather_main, *weather_icon, *weather_temp, *weather_condition;
    lv_obj_t *weather_hour_time[DESK_HOURS], *weather_hour_icon[DESK_HOURS], *weather_hour_temp[DESK_HOURS];
    lv_obj_t *weather_footer, *weather_empty;
    lv_img_dsc_t *weather_imgs[ARVEL_WEATHER_COUNT][2];

    /* panels */
    lv_obj_t *scenes_panel, *scenes_title, *scene_dots[2], *scene_prev, *scene_next;
    lv_obj_t *scene_tiles[SCENES_PER_PAGE], *scene_rings[SCENES_PER_PAGE];
    lv_obj_t *scene_thumbs[SCENES_PER_PAGE], *scene_names[SCENES_PER_PAGE];
    lv_obj_t *settings_panel, *tabs[TAB_COUNT], *tab_labels[TAB_COUNT], *settings_rule;
    row_slot_t rows[ROWS_PER_TAB];
    lv_obj_t *settings_nav, *settings_list;
    lv_obj_t *wifi_setup, *wifi_setup_title, *wifi_setup_key, *wifi_setup_url, *wifi_setup_status;
    lv_obj_t *wifi_setup_join, *wifi_setup_caption, *wifi_setup_hint;
    lv_obj_t *wifi_setup_buttons[3], *wifi_setup_labels[3];
    bool wifi_setup_visible;
    bool wifi_setup_failed;
    char wifi_setup_password[9];
    uint32_t wifi_setup_seconds, wifi_setup_checked;
    lv_obj_t *web_login, *web_login_title, *web_login_code, *web_login_caption;
    lv_obj_t *web_login_url, *web_login_status, *web_login_hint;
    lv_obj_t *web_login_buttons[3], *web_login_labels[3];
    bool web_login_visible, web_login_failed;
    char web_login_secret[7];
    uint32_t web_login_seconds;
    lv_obj_t *maintenance, *maintenance_title, *maintenance_detail, *maintenance_status;
    lv_obj_t *maintenance_buttons[3], *maintenance_labels[3];
    bool maintenance_visible, maintenance_requested, maintenance_failed;
    uint8_t maintenance_action; /* 0: menu; 1: restart confirmation; 2: reset confirmation */

    /* rail */
    lv_obj_t *scene_btn, *scene_icon, *chat_btn, *chat_icon, *mic_btn, *mic_icon, *stop_icon, *mute_slash;
    lv_obj_t *end_btn;
    lv_obj_t *wake_catcher;

    /* animation */
    track_t tracks[MAX_TRACKS];
    size_t track_count;
    size_t scene_track_start;
    bool group_on[G_COUNT];
    lv_timer_t *frame_timer;
    lv_timer_t *linger_timer;
    lv_timer_t *save_timer;
    uint32_t frame;

    /* state */
    ui_mode_t mode;
    buddy_ui_state_t state;
    bool busy;
    bool connected;
    bool clock_ready;
    int hour;
    char ssid[33];
    char station_ip[16];
    char buddy_name[33];
    char user_name[33];
    char model[129];
    char firmware[33];
    char storage_state[24], conversation_id[37], native_readiness[24];
    bool chat_configured;
    bool voice_configured;
    bool voice_live_available;
    bool voice_active;
    bool voice_muted;
    bool voice_connecting;
    bool mic_hold_handled;
    char detail[160];
    char notice[120];
    buddy_ui_prefs_t prefs;
    bool prefs_known;
    bool dirty;
    int tab;
    int scene_page;
    int chat_page;
    int wpm;

    /* conversation */
    size_t message_count;
    uint32_t message_ids[MAX_MESSAGES];
    bool message_assistant[MAX_MESSAGES];
    char *message_text[MAX_MESSAGES];
    char message_time[MAX_MESSAGES][6];
    tool_slot_t message_tools[MAX_MESSAGES][CHAT_CHIPS];
    uint8_t message_tool_count[MAX_MESSAGES];
    tool_slot_t live_tools[THINK_ROWS];   /* the latest calls of the reply in progress */
    size_t live_tool_count;
    uint32_t last_reply_id;
    bool fresh_reply;
    bool replying;            /* speaking layout is up, for TTS or a silent reply */
    bool reply_spoken;
    uint32_t speak_started;
    size_t speak_words_shown;

    /* tool pages */
    ui_mode_t reply_page;     /* page of the tool the reply in progress used */
    bool page_pinned;         /* opened by a tool result: stays up for the rest of the turn */
    size_t tool_results_seen; /* finished calls of the current turn already handled */
    bool desk_pending;        /* a page control is waiting on the desk tool */
    char page_notice[64];
    uint32_t page_notice_tick;
    bool timer_known;
    buddy_ui_timer_t timer;   /* label points at timer_text */
    char timer_text[129];
    uint32_t timer_seen_id, timer_done_tick;
    const char *done_actions[2];
    bool notes_ready;
    size_t note_count;
    note_slot_t *notes;       /* NOTE_ROWS slots in PSRAM, allocated with the first notes */
    bool calc_known;
    bool calc_conversion;
    char calc_expression[257];
    double calc_input, calc_result;
    int8_t calc_from, calc_to, calc_choice[3];
    int8_t calc_shown;        /* the unit picked on the page, or -1 for the result's */
    bool weather_known;
    char weather_location[97], weather_observed[6], weather_condition_text[49];
    int16_t weather_code;
    int weather_degrees, weather_feels, weather_high, weather_low;
    size_t weather_hours;
    struct {
        char time[6];
        int16_t code;
        int temp;
        uint8_t rain;
    } weather_forecast[DESK_HOURS];
} ui;

/* ------------------------------------------------------------------ helpers */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    return obj;
}

static lv_obj_t *tap_target(lv_obj_t *parent, lv_event_cb_t callback, void *data)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(obj, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, data);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, const char *value)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_label_set_text(obj, value);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    return obj;
}

static lv_obj_t *icon(lv_obj_t *parent, const lv_img_dsc_t *src)
{
    lv_obj_t *obj = lv_img_create(parent);
    lv_img_set_src(obj, src);
    lv_obj_set_style_img_recolor_opa(obj, LV_OPA_COVER, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    return obj;
}

static void place(lv_obj_t *obj, int x, int y, int w, int h)
{
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
}

static void hidden(lv_obj_t *obj, bool value)
{
    if(value) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void flex(lv_obj_t *obj, lv_flex_flow_t flow, int gap)
{
    lv_obj_set_flex_flow(obj, flow);
    lv_obj_set_style_pad_row(obj, gap, 0);
    lv_obj_set_style_pad_column(obj, gap, 0);
}

static lv_obj_t *spacer(lv_obj_t *parent)
{
    lv_obj_t *obj = box(parent);
    lv_obj_set_flex_grow(obj, 1);
    return obj;
}

/* CSS line-height in pixels for a label using this font. */
static void line_height(lv_obj_t *label, const lv_font_t *font, int px)
{
    lv_obj_set_style_text_line_space(label, px - lv_font_get_line_height(font), 0);
}

static void set_text(lv_obj_t *label, const char *value)
{
    if(!value) value = "";
    if(strcmp(lv_label_get_text(label), value) != 0) lv_label_set_text(label, value);
}

static int text_width(const char *value, const lv_font_t *font, int letter_space)
{
    lv_point_t size;
    lv_txt_get_size(&size, value, font, letter_space, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

/* Sizes a single-line label to its text, up to max_width (then "…"). Measures the
 * source string: LONG_DOT labels rewrite their own text when truncating. */
static void set_fitted(lv_obj_t *label, const char *value, const lv_font_t *font, int extra, int max_width)
{
    if(!value) value = "";
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_obj_set_width(label, LV_MIN(text_width(value, font, 0) + extra, max_width));
    set_text(label, value);
}

static void copy(char *target, size_t size, const char *value)
{
    snprintf(target, size, "%s", value ? value : "");
}

static void shadow(lv_obj_t *obj, int width, int offset, lv_opa_t opa, uint32_t color)
{
    lv_obj_set_style_shadow_width(obj, width, 0);
    lv_obj_set_style_shadow_ofs_y(obj, offset, 0);
    lv_obj_set_style_shadow_opa(obj, opa, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(color), 0);
}

/* Bitmaps and large tables go to PSRAM: LVGL's small allocations already use
 * the internal RAM Wi-Fi and TLS need. Freed with lv_mem_free (free). */
static void *psram_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(p) return p;
#endif
    return lv_mem_alloc(size);
}

/* ------------------------------------------------------------- PNG assets */

/* Decodes a PNG into an LVGL image in PSRAM; alpha keeps per-pixel opacity. */
static lv_img_dsc_t *decode_png(const uint8_t *png, size_t size, bool alpha)
{
    unsigned char *rgba = NULL;
    unsigned w = 0, h = 0;
    if(lodepng_decode32(&rgba, &w, &h, png, size) != 0 || !rgba) return NULL;
    const size_t px = alpha ? LV_IMG_PX_SIZE_ALPHA_BYTE : sizeof(lv_color_t);
    lv_img_dsc_t *img = psram_alloc(sizeof(lv_img_dsc_t) + (size_t)w * h * px);
    if(!img) {
        lv_mem_free(rgba);
        return NULL;
    }
    uint8_t *data = (uint8_t *)(img + 1);
    for(size_t i = 0; i < (size_t)w * h; ++i) {
        const unsigned char *s = rgba + i * 4;
        lv_color_t c = lv_color_make(s[0], s[1], s[2]);
        memcpy(data + i * px, &c, sizeof(lv_color_t));
        if(alpha) data[i * px + sizeof(lv_color_t)] = s[3];
    }
    lv_mem_free(rgba);
    memset(&img->header, 0, sizeof(img->header));
    img->header.cf = alpha ? LV_IMG_CF_TRUE_COLOR_ALPHA : LV_IMG_CF_TRUE_COLOR;
    img->header.w = w;
    img->header.h = h;
    img->data_size = (uint32_t)(w * h * px);
    img->data = data;
    return img;
}

/* ------------------------------------------------------------- animations */

static track_t *add_track(lv_obj_t *obj, track_group_t group, track_prop_t prop, const int16_t *values,
                          uint8_t count, uint16_t period, uint16_t begin)
{
    if(ui.track_count >= MAX_TRACKS) return NULL;
    track_t *t = &ui.tracks[ui.track_count++];
    memset(t, 0, sizeof(*t));
    t->obj = obj;
    t->group = group;
    t->prop = prop;
    t->values = values;
    t->count = count;
    t->period = period;
    t->begin = begin;
    t->divisor = 1;
    t->last = INT16_MIN;
    return t;
}

static int32_t sample(const track_t *t, uint32_t now)
{
    if(t->count == 1 || !t->period) return t->values[0];
    const uint32_t phase = (now + t->period - t->begin % t->period) % t->period;
    const int32_t f = (int32_t)(phase * 1000 / t->period);
    for(uint8_t i = 0; i + 1 < t->count; ++i) {
        const int32_t k0 = t->key_times ? t->key_times[i] : 1000 * i / (t->count - 1);
        const int32_t k1 = t->key_times ? t->key_times[i + 1] : 1000 * (i + 1) / (t->count - 1);
        if(f <= k1 || i + 2 == t->count) {
            const int32_t span = k1 - k0 ? k1 - k0 : 1;
            return t->values[i] + (t->values[i + 1] - t->values[i]) * (f - k0) / span;
        }
    }
    return t->values[t->count - 1];
}

static void apply_track(track_t *t, uint32_t now)
{
    const int32_t raw = sample(t, now);
    const int32_t value = (raw >= 0 ? raw + t->divisor / 2 : raw - t->divisor / 2) / t->divisor;
    if(value == t->last) return;
    t->last = (int16_t)value;
    switch(t->prop) {
        case P_OPA: lv_obj_set_style_opa(t->obj, (lv_opa_t)LV_CLAMP(0, value, 255), 0); break;
        case P_X: lv_obj_set_x(t->obj, t->base_x + value); break;
        case P_Y: lv_obj_set_y(t->obj, t->base_y + value); break;
        case P_H: lv_obj_set_height(t->obj, value); break;
        case P_TRANSLATE_Y: lv_obj_set_style_translate_y(t->obj, value, 0); break;
        case P_RADIUS:
            lv_obj_set_size(t->obj, value * 2, value * 2);
            lv_obj_set_pos(t->obj, t->base_x - value, t->base_y - value);
            break;
        case P_ANGLE: lv_img_set_angle(t->obj, (int16_t)(value < 0 ? value + 3600 : value)); break;
    }
}

static void frame_cb(lv_timer_t *timer);

/* ------------------------------------------------------------------ palette */

static void make_palette(bool dark)
{
    palette_t *p = &ui.pal;
    if(dark) {
        p->ink = lv_color_hex(0xF1EFE8); p->sub = lv_color_hex(0xB7C0BB); p->faint = lv_color_hex(0x6F7B80);
        p->accent = lv_color_hex(0x8FC3BB); p->bubble = lv_color_hex(0x161C24); p->bubble_opa = 224;
        p->rail = lv_color_hex(0x161C24); p->rail_opa = 199; p->rail_ink = lv_color_hex(0xE6E9E4);
        p->chip = lv_color_hex(0xF2EEEA); p->chip_opa = 36; p->chip_ink = lv_color_hex(0xEBD6CB);
        p->user_bg = lv_color_hex(0x8FC3BB); p->user_opa = 46; p->user_ink = lv_color_hex(0xE3EFEC);
        p->rule = lv_color_hex(0xFFFFFF); p->rule_opa = 23; p->track_off = lv_color_hex(0x3A4650);
        p->seg_on = lv_color_hex(0x2E3A44); p->ring_gap = lv_color_hex(0x16212A); p->active_ink = lv_color_hex(0x16212A);
        p->tool_chip = lv_color_hex(0xFFFFFF); p->tool_chip_opa = 20;
        p->ring_track = lv_color_hex(0xFFFFFF); p->ring_track_opa = 31;
        p->dot_off = lv_color_hex(0xFFFFFF); p->dot_off_opa = 41;
        p->note_bg = lv_color_hex(NOTE_BAR); p->note_bg_opa = 31;
        p->urgent_bg = lv_color_hex(TIMER_RED); p->urgent_opa = 51; p->urgent_ink = lv_color_hex(0xF4B3A6);
        p->box_border = lv_color_hex(0x6F7B80);
    } else {
        p->ink = lv_color_hex(0x252724); p->sub = lv_color_hex(0x5C6660); p->faint = lv_color_hex(0xA7AEA9);
        p->accent = lv_color_hex(0x456B67); p->bubble = lv_color_hex(0xFBFBF9); p->bubble_opa = 242;
        p->rail = lv_color_hex(0xFBFBF9); p->rail_opa = 230; p->rail_ink = lv_color_hex(0x3B423C);
        p->chip = lv_color_hex(0xF2EEEA); p->chip_opa = 255; p->chip_ink = lv_color_hex(0x725A50);
        p->user_bg = lv_color_hex(0xEDF4F3); p->user_opa = 255; p->user_ink = lv_color_hex(0x2F4F4B);
        p->rule = lv_color_hex(0xE9EAE6); p->rule_opa = 255; p->track_off = lv_color_hex(0xCDD2CD);
        p->seg_on = lv_color_hex(0xFFFFFF); p->ring_gap = lv_color_hex(0xFBFBF9); p->active_ink = lv_color_hex(0xFFFFFF);
        p->tool_chip = lv_color_hex(0xF2F2EE); p->tool_chip_opa = 255;
        p->ring_track = lv_color_hex(0xEEE8E3); p->ring_track_opa = 255;
        p->dot_off = lv_color_hex(0xE2DED9); p->dot_off_opa = 255;
        p->note_bg = lv_color_hex(0xFBF3DF); p->note_bg_opa = 255;
        p->urgent_bg = lv_color_hex(DONE_HALO); p->urgent_opa = 255; p->urgent_ink = lv_color_hex(0xA2402F);
        p->box_border = lv_color_hex(0x9AA39E);
    }
}

static void bg(lv_obj_t *obj, lv_color_t color, lv_opa_t opa)
{
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
}

static void ink(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_text_color(obj, color, 0);
}

static void tint(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_img_recolor(obj, color, 0);
}

/* ------------------------------------------------------------ model helpers */

static const char *last_message(bool assistant, uint32_t *id)
{
    for(size_t i = ui.message_count; i > 0; --i) {
        if(ui.message_assistant[i - 1] == assistant) {
            if(id) *id = ui.message_ids[i - 1];
            return ui.message_text[i - 1];
        }
    }
    return NULL;
}

/* Question/answer pairs for the chat log: each user message with the reply after it. */
static size_t chat_pairs(int16_t (*pairs)[2])
{
    size_t count = 0;
    for(size_t i = 0; i < ui.message_count; ++i) {
        if(!ui.message_assistant[i]) {
            pairs[count][0] = (int16_t)i;
            pairs[count][1] = i + 1 < ui.message_count && ui.message_assistant[i + 1] ? (int16_t)(i + 1) : -1;
            if(pairs[count][1] >= 0) ++i;
        } else {
            pairs[count][0] = -1;
            pairs[count][1] = (int16_t)i;
        }
        ++count;
    }
    return count;
}

static const char *scene_name(void)
{
    return arvel_scenes[ui.scene].name;
}

static void copy_tool(tool_slot_t *slot, const buddy_ui_tool_t *tool)
{
    slot->kind = tool->kind < ARVEL_TOOL_COUNT ? (uint8_t)tool->kind : BUDDY_TOOL_MCP;
    slot->state = tool->state;
    copy(slot->verb, sizeof(slot->verb), tool->verb);
    copy(slot->target, sizeof(slot->target), tool->target);
    copy(slot->id, sizeof(slot->id), tool->id);
    copy(slot->error, sizeof(slot->error), tool->error);
}

/* A full-colour icon decoded on first use: size 0 is 22px (54px weather), size 1 is 20px (26px). */
static void set_icon_png(lv_obj_t *obj, lv_img_dsc_t **cache, const arvel_tool_icon_t *icon, int size)
{
    if(!*cache) *cache = decode_png(icon->sizes[size].png, icon->sizes[size].png_size, true);
    if(*cache) lv_img_set_src(obj, *cache);
}

static void set_tool_icon(lv_obj_t *obj, uint8_t kind, int size)
{
    set_icon_png(obj, &ui.tool_imgs[kind][size], &arvel_tool_icons[kind], size);
}

/* Shows a tool call in a thinking row (size 0, 22px) or chat chip (size 1, 20px). */
static void show_tool(tool_view_t *v, const tool_slot_t *slot, int size, int target_max)
{
    set_tool_icon(v->icon, slot->kind, size);
    static const char *states[] = {"Running", "Succeeded", "Failed", "Unknown", "Cancelled", "Partial"};
    const unsigned state = slot->state >= BUDDY_TOOL_RUNNING && slot->state <= BUDDY_TOOL_PARTIAL ? slot->state : BUDDY_TOOL_UNKNOWN;
    char label[48], detail[320];
    snprintf(label, sizeof(label), "%s · %s", slot->verb, states[state]);
    set_text(v->verb, label);
    snprintf(detail, sizeof(detail), "%s%s%s%s%s", slot->error,
             slot->error[0] && slot->target[0] ? " · " : "", slot->target,
             slot->id[0] && (slot->error[0] || slot->target[0]) ? " · " : "", slot->id);
    hidden(v->target, !detail[0]);
    set_fitted(v->target, detail, size ? M11 : M12, 1, target_max);
    if(v->spinner) hidden(v->spinner, state != BUDDY_TOOL_RUNNING);
    if(v->check) hidden(v->check, state != BUDDY_TOOL_SUCCEEDED);
    hidden(v->row, false);
}

/* ------------------------------------------------------------------ refresh */

static void refresh_status(void);
static void refresh_page(void);
static void apply_page_colors(void);
static void ensure_page(ui_mode_t mode);
static void free_scene_thumbs(void);
static void refresh_chat(void);
static void refresh_scenes(void);
static void refresh_settings(void);
static void refresh_speaking(bool all_words);
static void set_mode(ui_mode_t mode);

static void apply_colors(void)
{
    const palette_t *p = &ui.pal;
    lv_obj_t *panels[] = {ui.bubble, ui.scenes_panel, ui.settings_panel};
    for(size_t i = 0; i < 3; ++i) {
        bg(panels[i], p->bubble, p->bubble_opa);
        ink(panels[i], p->ink);
    }
    ink(ui.wifi_setup_title, p->ink);
    ink(ui.wifi_setup_join, p->ink);
    ink(ui.wifi_setup_key, p->ink);
    ink(ui.wifi_setup_caption, p->sub);
    ink(ui.wifi_setup_url, p->ink);
    ink(ui.wifi_setup_hint, p->sub);
    bg(ui.wifi_setup_status, p->user_bg, p->user_opa);
    ink(ui.wifi_setup_status, p->user_ink);
    for(size_t i = 0; i < 3; ++i) {
        bg(ui.wifi_setup_buttons[i], i == 0 ? p->user_bg : p->chip,
           i == 0 ? p->user_opa : i == 2 ? LV_OPA_TRANSP : p->chip_opa);
        ink(ui.wifi_setup_labels[i], i == 0 ? p->user_ink : p->chip_ink);
        lv_obj_set_style_border_color(ui.wifi_setup_buttons[i], p->rule, 0);
        lv_obj_set_style_outline_color(ui.wifi_setup_buttons[i], p->accent, LV_STATE_FOCUS_KEY);
    }
    ink(ui.web_login_title, p->ink);
    ink(ui.web_login_code, p->ink);
    ink(ui.web_login_caption, p->sub);
    ink(ui.web_login_url, p->ink);
    ink(ui.web_login_hint, p->sub);
    bg(ui.web_login_status, p->user_bg, p->user_opa);
    ink(ui.web_login_status, p->user_ink);
    for(size_t i = 0; i < 3; ++i) {
        bg(ui.web_login_buttons[i], i == 0 ? p->user_bg : p->chip,
           i == 0 ? p->user_opa : i == 2 ? LV_OPA_TRANSP : p->chip_opa);
        ink(ui.web_login_labels[i], i == 0 ? p->user_ink : p->chip_ink);
        lv_obj_set_style_border_color(ui.web_login_buttons[i], p->accent, 0);
        lv_obj_set_style_outline_color(ui.web_login_buttons[i], p->accent, LV_STATE_FOCUS_KEY);
    }
    ink(ui.maintenance_title, p->ink);
    ink(ui.maintenance_detail, p->sub);
    ink(ui.maintenance_status, p->sub);
    for(size_t i = 0; i < 3; ++i) {
        bg(ui.maintenance_buttons[i], p->chip, p->chip_opa);
        ink(ui.maintenance_labels[i], p->chip_ink);
    }
    tint(ui.tail, p->bubble);
    lv_obj_set_style_img_recolor_opa(ui.tail, LV_OPA_COVER, 0);
    lv_obj_set_style_img_opa(ui.tail, p->bubble_opa, 0);
    bg(ui.pill, p->rail, p->rail_opa);
    ink(ui.pill, p->ink);
    for(uint32_t i = 0; i < lv_obj_get_child_cnt(ui.pill); ++i) {
        lv_obj_t *child = lv_obj_get_child(ui.pill, i);
        if(lv_obj_check_type(child, &lv_img_class)) tint(child, p->ink);
    }
    ink(ui.status_label, p->sub);
    ink(ui.model_label, p->sub);
    ink(ui.idle_sub, p->sub);
    for(size_t i = 0; i < SUGGESTIONS; ++i) {
        bg(ui.suggest[i], p->tool_chip, p->tool_chip_opa);
        ink(ui.suggest[i], p->ink);
    }
    apply_page_colors();
    ink(ui.think_quote, p->sub);
    for(uint32_t i = 0; i < lv_obj_get_child_cnt(ui.think_footer); ++i) {
        lv_obj_t *child = lv_obj_get_child(ui.think_footer, i);
        if(lv_obj_check_type(child, &lv_img_class)) tint(child, p->sub);
        else ink(child, p->sub);
    }
    for(size_t i = 0; i < 3; ++i) bg(ui.think_dots[i], p->accent, LV_OPA_COVER);
    for(size_t i = 0; i < THINK_ROWS + CHAT_CHIPS; ++i) {
        const tool_view_t *v = i < THINK_ROWS ? &ui.think_rows[i] : &ui.chat_chips[i - THINK_ROWS];
        ink(v->verb, p->ink);
        ink(v->target, p->sub);
        if(v->spinner) lv_obj_set_style_arc_color(v->spinner, p->accent, LV_PART_INDICATOR);
        if(v->check) tint(v->check, p->accent);
        if(i >= THINK_ROWS) bg(v->row, p->tool_chip, p->tool_chip_opa);
    }
    ink(lv_obj_get_child(lv_obj_get_child(ui.listen_box, -1), -1), p->sub);
    ink(ui.speak_faint, p->faint);
    ink(ui.speak_ink, p->ink);
    ink(ui.live_role, p->sub);
    ink(ui.live_words, p->ink);
    ink(ui.live_empty, p->ink);
    ink(ui.live_hint, p->sub);
    for(uint32_t i = 0; i < lv_obj_get_child_cnt(ui.speak_icons); ++i) tint(lv_obj_get_child(ui.speak_icons, i), p->accent);
    ink(ui.speak_hint, p->sub);
    ink(ui.chat_time, p->sub);
    bg(ui.chat_dot, p->accent, LV_OPA_COVER);
    lv_obj_t *chat_buttons[] = {ui.chat_older, ui.chat_newer, ui.scene_prev, ui.scene_next};
    for(size_t i = 0; i < 4; ++i) {
        bg(chat_buttons[i], p->chip, p->chip_opa);
        tint(lv_obj_get_child(chat_buttons[i], 0), p->ink);
    }
    bg(ui.chat_q, p->user_bg, p->user_opa);
    bg(ui.chat_q_corner, p->user_bg, p->user_opa);
    ink(ui.chat_q_label, p->user_ink);
    lv_obj_set_style_border_color(ui.chat_a, p->accent, 0);
    bg(ui.settings_rule, p->rule, p->rule_opa);
    for(size_t i = 0; i < ROWS_PER_TAB; ++i) {
        row_slot_t *r = &ui.rows[i];
        lv_obj_set_style_border_color(r->row, p->rule, 0);
        lv_obj_set_style_border_opa(r->row, p->rule_opa, 0);
        for(uint32_t j = 0; j < 3; ++j) bg(lv_obj_get_child(r->stepper, j == 1 ? 2 : 0), p->chip, p->chip_opa);
        bg(r->seg, p->chip, p->chip_opa);
        ink(r->value_label, p->sub);
        tint(lv_obj_get_child(r->value, 1), p->sub);
    }
    lv_obj_set_style_text_color(ui.screen, p->ink, 0);
    refresh_status();
    refresh_chat();
    refresh_scenes();
    refresh_settings();
    set_mode(ui.mode);
}

/* Status row, idle/thinking texts and the clock pill. */
static void refresh_status(void)
{
    const palette_t *p = &ui.pal;
    static const char *const labels[] = {"READY", "LISTENING", "THINKING", "SPEAKING", "TODAY", "SCENES", "SETTINGS",
                                         "TIMER", "TIMER", "TO-DO", "CALCULATOR", "WEATHER"};
    uint32_t dot = READY_GREEN;
    const char *label = labels[ui.mode];
    if(ui.voice_active && ui.mode != MODE_CHAT && ui.mode != MODE_SCENES && ui.mode != MODE_SETTINGS) {
        if(!ui.connected) label = "OFFLINE";
        else if(ui.voice_muted) label = ui.voice_connecting ? "MUTED · CONNECTING" : "MIC MUTED";
        else if(ui.voice_connecting) label = "CONNECTING";
        else if(ui.state == BUDDY_THINKING) label = "WORKING";
        else if(ui.state == BUDDY_SPEAKING) label = "SPEAKING";
        else label = "LISTENING";
    }
    if(ui.mode == MODE_IDLE && ui.state == BUDDY_ERROR) { label = "ERROR"; dot = MIC_RED; }
    else if(ui.mode == MODE_LISTENING && !ui.voice_active) dot = MIC_RED;
    else if(ui.mode == MODE_THINKING) dot = THINK_AMBER;
    else if(ui.mode == MODE_SPEAKING && !ui.reply_spoken && !ui.voice_active) label = "REPLY";
    lv_obj_set_style_bg_color(ui.status_dot, ui.mode == MODE_SPEAKING ? p->accent : lv_color_hex(dot), 0);
    set_text(ui.status_label, label);
    set_fitted(ui.model_label, ui.model, F11M, 1, 200);

    char line[200];
    if(ui.state == BUDDY_ERROR) {
        set_text(ui.idle_title, "Request needs attention");
    } else {
        const char *part = !ui.clock_ready ? "Hello" : ui.hour < 12 ? "Morning" : ui.hour < 18 ? "Afternoon" : "Evening";
        if(ui.user_name[0]) snprintf(line, sizeof(line), "%s, %s.", part, ui.user_name);
        else snprintf(line, sizeof(line), "%s%s.", ui.clock_ready ? "Good " : "", ui.clock_ready ? part : "Hello there");
        if(ui.clock_ready && !ui.user_name[0]) line[5] = (char)(line[5] - 'A' + 'a');
        set_text(ui.idle_title, line);
    }
    const char *sub;
    if(ui.notice[0]) sub = ui.notice;
    else if(ui.state == BUDDY_ERROR && ui.detail[0]) sub = ui.detail;
    else if(strncmp(ui.detail, "Voice chat ended.", 17) == 0) sub = ui.detail;
    else if(!ui.connected) sub = "Offline — open Settings > Device > Wi-Fi for local setup.";
    else if(!ui.chat_configured && !ui.voice_live_available) sub = "Open browser Settings to choose a conversation model and configure its credentials.";
    else if(ui.voice_live_available) sub = "Tap mic to start or mute · tap End to stop.";
    else if(!ui.voice_configured) sub = "Open browser Settings to configure the selected conversation transport.";
    else sub = "Tap the mic to talk — I’m right here if you need a hand.";
    set_text(ui.idle_sub, sub);

    /* The tool shortcuts sit under a greeting of at most two lines. */
    lv_point_t sub_size;
    lv_txt_get_size(&sub_size, sub, F14R, 0, 0, 364, LV_TEXT_FLAG_NONE);
    hidden(ui.suggest_row, sub_size.y > 2 * lv_font_get_line_height(F14R));

    const char *last = last_message(false, NULL);

    const char *title = "Thinking it over";
    if(ui.state == BUDDY_CLOCK_SYNC) title = "Setting the clock";
    else if(ui.state == BUDDY_TRANSCRIBING) title = "Catching your words";
    else if(strncmp(ui.detail, "Starting a new chat", 19) == 0) title = "Starting a new chat";
    set_text(ui.think_title, title);
    /* Live tool calls replace the quoted request once the agent starts using tools. */
    const bool tools = ui.state == BUDDY_THINKING && ui.live_tool_count;
    hidden(ui.think_tools, !tools);
    /* The list takes the spacer's place: one flex gap fewer keeps the footer inside the bubble. */
    hidden(ui.think_spacer, tools);
    for(size_t i = 0; i < THINK_ROWS; ++i) {
        if(tools && i < ui.live_tool_count) show_tool(&ui.think_rows[i], &ui.live_tools[i], 0, 200);
        else hidden(ui.think_rows[i].row, true);
    }
    const char *quote = ui.state == BUDDY_THINKING && !tools ? last : NULL;
    hidden(ui.think_quote, !quote || strncmp(ui.detail, "Starting a new chat", 19) == 0);
    if(quote) {
        snprintf(line, sizeof(line), "“%s”", quote);
        set_text(ui.think_quote, line);
    }
    snprintf(line, sizeof(line), "Using %s through your API", ui.model[0] ? ui.model : "your model");
    set_text(lv_obj_get_child(ui.think_footer, 1), line);
}

static void refresh_live(void)
{
    const bool caption = ui.prefs.captions && lv_label_get_text(ui.live_words)[0];
    hidden(ui.live_role, !caption);
    hidden(ui.live_scroll, !caption);
    hidden(ui.live_empty, caption);
    set_text(ui.live_empty, !ui.connected ? "Voice chat is offline" :
             ui.voice_connecting ? "Opening voice chat…" :
             ui.voice_muted ? "Microphone muted" :
             ui.state == BUDDY_THINKING ? "Working on your request" :
             ui.state == BUDDY_SPEAKING ? "Playing voice reply" : "Voice chat is open");
    set_text(ui.live_hint, !ui.connected ? "Offline · tap End to stop" :
             ui.voice_muted ? "Mic muted · tap mic to unmute · End to stop" :
             ui.voice_connecting ? "Connecting · tap End to stop" : "Mic on · tap mic to mute · End to stop");
}

static void refresh_speaking(bool all_words)
{
    const char *reply = last_message(true, NULL);
    if(!reply) reply = "";
    set_text(ui.speak_faint, reply);
    size_t shown = strlen(reply);
    if(!all_words && ui.prefs.captions) {
        /* Reveal whole words: the prefix wraps exactly like the full text underneath. */
        size_t words = ui.speak_words_shown, i = 0;
        while(reply[i] == ' ' || reply[i] == '\n') ++i;
        while(words && reply[i]) {
            while(reply[i] && reply[i] != ' ' && reply[i] != '\n') ++i;
            if(--words) while(reply[i] == ' ' || reply[i] == '\n') ++i;
        }
        shown = i;
    }
    static char buffer[TEXT_MAX];
    memcpy(buffer, reply, shown);
    buffer[shown] = '\0';
    set_text(ui.speak_ink, buffer);
    hidden(ui.speak_faint, !ui.prefs.captions || all_words);
    if(!all_words && ui.prefs.captions) {
        lv_obj_update_layout(ui.speak_scroll);
        const lv_coord_t visible = lv_obj_get_height(ui.speak_scroll);
        const lv_coord_t written = lv_obj_get_height(ui.speak_ink);
        lv_obj_scroll_to_y(ui.speak_scroll, written > visible ? written - visible : 0, LV_ANIM_ON);
    }
    hidden(ui.speak_icons, !ui.reply_spoken || ui.state != BUDDY_SPEAKING);
    set_text(ui.speak_hint, ui.state == BUDDY_SPEAKING ? "Speaking · tap the mic to interrupt" : "Tap the mic to reply");
}

static void refresh_chat(void)
{
    int16_t pairs[MAX_MESSAGES][2];
    const size_t count = chat_pairs(pairs);
    if(ui.chat_page < 0 || ui.chat_page >= (int)count) ui.chat_page = count ? (int)count - 1 : 0;
    for(size_t i = 0; i < MAX_MESSAGES; ++i) {
        hidden(ui.page_dots[i], i >= count);
        if(i >= count) continue;
        const bool on = (int)i == ui.chat_page;
        lv_obj_set_width(ui.page_dots[i], on ? 14 : 6);
        bg(ui.page_dots[i], on ? ui.pal.accent : ui.pal.faint, LV_OPA_COVER);
    }
    lv_obj_set_style_opa(ui.chat_older, ui.chat_page > 0 ? LV_OPA_COVER : 89, 0);
    lv_obj_set_style_opa(ui.chat_newer, ui.chat_page + 1 < (int)count ? LV_OPA_COVER : 89, 0);
    char header[32];
    const char *q = NULL, *a = NULL, *time = "";
    size_t tools = 0;
    int16_t ai = -1;
    if(count) {
        const int16_t qi = pairs[ui.chat_page][0];
        ai = pairs[ui.chat_page][1];
        if(qi >= 0) { q = ui.message_text[qi]; time = ui.message_time[qi]; }
        if(ai >= 0) { a = ui.message_text[ai]; if(!time[0]) time = ui.message_time[ai]; tools = ui.message_tool_count[ai]; }
    }
    hidden(ui.chat_tools, !tools);
    for(size_t i = 0; i < CHAT_CHIPS; ++i) {
        if(i < tools) show_tool(&ui.chat_chips[i], &ui.message_tools[ai][i], 1, 120);
        else hidden(ui.chat_chips[i].row, true);
    }
    snprintf(header, sizeof(header), time[0] ? "TODAY · %s" : "TODAY", time);
    set_text(ui.chat_time, header);
    hidden(ui.chat_q_row, !q);
    if(q) {
        set_text(ui.chat_q_label, q);
        /* Right-aligned bubble, at most 80% of the content width. */
        const int max_text = 291 - 20;
        int w = text_width(q, F13R, 0);
        if(w > max_text || strchr(q, '\n')) w = max_text;
        lv_obj_set_width(ui.chat_q_label, w);
        lv_obj_update_layout(ui.chat_q_label);
        const int h = lv_obj_get_height(ui.chat_q_label);
        lv_obj_set_size(ui.chat_q, w + 20, h + 10);
        lv_obj_set_pos(ui.chat_q_corner, w + 20 - 12, h + 10 - 12);
    }
    set_text(ui.chat_a, a ? a : count ? "Waiting for the reply…" : "No conversation yet. Tap the mic to start one.");
    lv_obj_scroll_to_y(ui.chat_content, 0, LV_ANIM_OFF);
}

static void refresh_scenes(void)
{
    char title[96];
    snprintf(title, sizeof(title), "Where should %s hang out?", ui.buddy_name[0] ? ui.buddy_name : "Arvel");
    set_text(ui.scenes_title, title);
    const int pages = (ARVEL_SCENE_COUNT + SCENES_PER_PAGE - 1) / SCENES_PER_PAGE;
    for(int i = 0; i < 2; ++i) {
        const bool on = i == ui.scene_page;
        hidden(ui.scene_dots[i], i >= pages);
        lv_obj_set_width(ui.scene_dots[i], on ? 14 : 6);
        bg(ui.scene_dots[i], on ? ui.pal.accent : ui.pal.faint, LV_OPA_COVER);
    }
    lv_obj_set_style_opa(ui.scene_prev, ui.scene_page > 0 ? LV_OPA_COVER : 89, 0);
    lv_obj_set_style_opa(ui.scene_next, ui.scene_page < pages - 1 ? LV_OPA_COVER : 89, 0);
    for(int i = 0; i < SCENES_PER_PAGE; ++i) {
        const int index = ui.scene_page * SCENES_PER_PAGE + i;
        hidden(ui.scene_tiles[i], index >= ARVEL_SCENE_COUNT);
        if(index >= ARVEL_SCENE_COUNT) continue;
        if(!ui.thumbs[index]) {
            ui.thumbs[index] = decode_png(arvel_scenes[index].thumb_png, arvel_scenes[index].thumb_png_size, true);
        }
        if(ui.thumbs[index]) lv_img_set_src(ui.scene_thumbs[i], ui.thumbs[index]);
        const bool on = (size_t)index == ui.scene;
        set_text(ui.scene_names[i], arvel_scenes[index].name);
        lv_obj_set_style_text_font(ui.scene_names[i], on ? F12B : F12M, 0);
        ink(ui.scene_names[i], ui.pal.ink);
        /* Selected: a 2px gap in the panel colour, then a 2px accent ring. */
        lv_obj_set_style_outline_width(ui.scene_rings[i], on ? 2 : 1, 0);
        lv_obj_set_style_outline_pad(ui.scene_rings[i], on ? 2 : 0, 0);
        lv_obj_set_style_outline_color(ui.scene_rings[i], on ? ui.pal.accent : lv_color_black(), 0);
        lv_obj_set_style_outline_opa(ui.scene_rings[i], on ? LV_OPA_COVER : 20, 0);
    }
}

/* Scene-picker thumbnails are decoded on demand; drop them when the picker
 * closes so their PSRAM (12 decoded thumbs) is not held for the whole run.
 * refresh_scenes() re-decodes on the next open. */
static void free_scene_thumbs(void)
{
    for(size_t i = 0; i < ARVEL_SCENE_COUNT; ++i) {
        if(!ui.thumbs[i]) continue;
        for(int t = 0; t < SCENES_PER_PAGE; ++t)
            if(lv_img_get_src(ui.scene_thumbs[t]) == ui.thumbs[i]) lv_img_set_src(ui.scene_thumbs[t], NULL);
        lv_mem_free(ui.thumbs[i]);
        ui.thumbs[i] = NULL;
    }
}

static void set_toggle(row_slot_t *r, bool on, bool enabled)
{
    bg(r->toggle, on ? ui.pal.accent : ui.pal.track_off, LV_OPA_COVER);
    lv_obj_set_x(r->knob, on ? 19 : 3);
    lv_obj_set_style_opa(r->toggle, enabled ? LV_OPA_COVER : 115, 0);
    if(enabled) lv_obj_clear_state(r->toggle, LV_STATE_DISABLED);
    else lv_obj_add_state(r->toggle, LV_STATE_DISABLED);
}

static void set_seg(row_slot_t *r, const char *const *opts, int selected)
{
    for(int i = 0; i < 3; ++i) {
        const bool on = i == selected;
        set_text(r->seg_labels[i], opts[i]);
        lv_obj_set_style_text_font(r->seg_labels[i], on ? F12S : F12M, 0);
        ink(r->seg_labels[i], ui.pal.ink);
        bg(r->seg_opts[i], ui.pal.seg_on, on ? LV_OPA_COVER : LV_OPA_TRANSP);
        lv_obj_set_style_shadow_opa(r->seg_opts[i], on ? 31 : 0, 0);
    }
}

static void set_value(row_slot_t *r, const char *value)
{
    set_fitted(r->value_label, value, F13R, 1, 170);
}

static void refresh_wifi_setup(void)
{
    const bool active = ui.wifi_setup_seconds != 0;
    const bool ready = active && ui.wifi_setup_password[0] && !ui.wifi_setup_failed;
    char value[32];
    if(active) {
        snprintf(value, sizeof(value), "Active · %02u:%02u left",
                 (unsigned)(ui.wifi_setup_seconds / 60), (unsigned)(ui.wifi_setup_seconds % 60));
        set_text(ui.wifi_setup_status, value);
    } else {
        set_text(ui.wifi_setup_status, ui.wifi_setup_failed ? "Open failed" : "Closed · 00:00");
    }
    set_text(ui.wifi_setup_key, ready ? ui.wifi_setup_password :
             active ? "Refresh failed" : ui.wifi_setup_failed ? "Setup unavailable" : "Setup closed");
    set_text(ui.wifi_setup_url, ready ? "2  Open 192.168.4.1" : "");
    set_text(ui.wifi_setup_hint, ready ?
             "Finish Wi-Fi onboarding;\nthen Device > Web login for keys." :
             active ? "Tap Retry when Arvel is idle,\nor Stop setup to close the window." :
             ui.wifi_setup_failed ? "Tap Retry to open a new\nfive-minute setup window." :
             "New password opens a fresh\nfive-minute setup window.");
    hidden(ui.wifi_setup_join, !ready);
    hidden(ui.wifi_setup_caption, !ready);
    hidden(ui.wifi_setup_url, !ready);
    set_text(ui.wifi_setup_labels[0], ui.wifi_setup_failed ? "Retry" : "New password");
    if(active) lv_obj_clear_state(ui.wifi_setup_buttons[1], LV_STATE_DISABLED);
    else lv_obj_add_state(ui.wifi_setup_buttons[1], LV_STATE_DISABLED);
}

static void poll_wifi_setup(void)
{
    if(!ui.wifi_setup_seconds && !ui.wifi_setup_password[0]) return;
    const uint32_t seconds = ui.callbacks.wifi_setup_remaining ?
        ui.callbacks.wifi_setup_remaining(ui.callbacks.context) : 0;
    if(seconds == ui.wifi_setup_seconds) return;
    ui.wifi_setup_seconds = seconds;
    if(!seconds) memset(ui.wifi_setup_password, 0, sizeof(ui.wifi_setup_password));
    refresh_wifi_setup();
}

static void start_wifi_setup(void)
{
    memset(ui.wifi_setup_password, 0, sizeof(ui.wifi_setup_password));
    const bool opened = ui.callbacks.wifi_setup_start &&
        ui.callbacks.wifi_setup_start(ui.wifi_setup_password, sizeof(ui.wifi_setup_password), ui.callbacks.context);
    ui.wifi_setup_failed = !opened;
    ui.wifi_setup_seconds = ui.callbacks.wifi_setup_remaining ?
        ui.callbacks.wifi_setup_remaining(ui.callbacks.context) : 0;
    if(!opened || !ui.wifi_setup_seconds) memset(ui.wifi_setup_password, 0, sizeof(ui.wifi_setup_password));
    refresh_wifi_setup();
}

static void on_wifi_setup(lv_event_t *e)
{
    const intptr_t action = (intptr_t)lv_event_get_user_data(e);
    if(action == 0) start_wifi_setup();
    else if(action == 1) {
        if(ui.callbacks.wifi_setup_stop) ui.callbacks.wifi_setup_stop(ui.callbacks.context);
        ui.wifi_setup_seconds = 0;
        ui.wifi_setup_failed = false;
        memset(ui.wifi_setup_password, 0, sizeof(ui.wifi_setup_password));
        refresh_wifi_setup();
    } else {
        ui.wifi_setup_visible = false;
        refresh_settings();
    }
}

static void clear_web_login_code(void)
{
    memset(ui.web_login_secret, 0, sizeof(ui.web_login_secret));
    /* Erase the LVGL-owned copy too, even while the subview is hidden. */
    char *label_code = lv_label_get_text(ui.web_login_code);
    if(label_code) {
        volatile char *p = label_code;
        while(*p) *p++ = '\0';
    }
    set_text(ui.web_login_code, "");
}

static void refresh_web_login(void)
{
    const bool active = ui.web_login_seconds && ui.web_login_secret[0];
    char value[64];
    if(active) {
        snprintf(value, sizeof(value), "Approve · %02u:%02u left",
                 (unsigned)(ui.web_login_seconds / 60), (unsigned)(ui.web_login_seconds % 60));
        set_text(ui.web_login_status, value);
    } else {
        set_text(ui.web_login_status, ui.web_login_failed ? "Approval unavailable" : "Approval closed · 00:00");
    }
    set_text(ui.web_login_code, active ? ui.web_login_secret : "");
    hidden(ui.web_login_caption, !active);
    if(ui.connected && ui.station_ip[0] && strcmp(ui.station_ip, "0.0.0.0")) {
        snprintf(value, sizeof(value), "http://%s/settings", ui.station_ip);
        set_text(ui.web_login_url, value);
    } else {
        set_text(ui.web_login_url, "Connect home Wi-Fi first.");
    }
    set_text(ui.web_login_hint, active ?
             "Create or recover the web login; saving replaces the owner\nand revokes all browser sessions. Back keeps approval open." :
             ui.web_login_failed ?
             "Could not open approval. Tap New code to try again.\nWi-Fi is unchanged; existing web access is kept." :
             "Code cleared after use, expiry or cancel. New code allows\ncreating or recovering the login and revokes sessions when saved.");
    if(active) lv_obj_clear_state(ui.web_login_buttons[1], LV_STATE_DISABLED);
    else lv_obj_add_state(ui.web_login_buttons[1], LV_STATE_DISABLED);
}

static void poll_web_login(void)
{
    if(!ui.web_login_seconds && !ui.web_login_secret[0]) return;
    const uint32_t seconds = ui.callbacks.web_login_remaining ?
        ui.callbacks.web_login_remaining(ui.callbacks.context) : 0;
    if(seconds == ui.web_login_seconds) return;
    ui.web_login_seconds = seconds;
    if(!seconds) clear_web_login_code();
    refresh_web_login();
}

static void start_web_login(void)
{
    clear_web_login_code();
    const bool opened = ui.callbacks.web_login_start &&
        ui.callbacks.web_login_start(ui.web_login_secret, sizeof(ui.web_login_secret), ui.callbacks.context);
    ui.web_login_seconds = ui.callbacks.web_login_remaining ?
        ui.callbacks.web_login_remaining(ui.callbacks.context) : 0;
    ui.web_login_failed = !opened || !ui.web_login_seconds;
    if(ui.web_login_failed) clear_web_login_code();
    refresh_web_login();
}

static void on_web_login(lv_event_t *e)
{
    const intptr_t action = (intptr_t)lv_event_get_user_data(e);
    if(action == 0) start_web_login();
    else if(action == 1) {
        if(ui.callbacks.web_login_stop) ui.callbacks.web_login_stop(ui.callbacks.context);
        ui.web_login_seconds = 0;
        ui.web_login_failed = false;
        clear_web_login_code();
        refresh_web_login();
    } else {
        /* Back preserves only the bounded five-minute physical approval. */
        ui.web_login_visible = false;
        refresh_settings();
    }
}

static void refresh_maintenance(void)
{
    const bool reset = ui.maintenance_action == 2;
    char firmware[64];
    snprintf(firmware, sizeof(firmware), "Firmware: %s", ui.firmware[0] ? ui.firmware : "Unknown");
    set_text(ui.maintenance_title, !ui.maintenance_action ? "Device actions" :
             ui.maintenance_requested ? (reset ? "Factory reset requested" : "Restart requested") :
             (reset ? "Factory reset device?" : "Restart device?"));
    set_text(ui.maintenance_detail, !ui.maintenance_action ?
             "Choose an action. Nothing happens until you confirm on this display." :
             reset ? "Erase saved settings, not SD history (even on an absent card). Firmware credentials remain." :
             "Restart the device. Saved settings are kept; stop current activity first.");
    set_text(ui.maintenance_status, ui.maintenance_failed ?
             "Request was not accepted. Stop current activity and try again." :
             ui.maintenance_requested ? "Request accepted; waiting for device restart." :
             ui.maintenance_action ? "Cancel goes back without changing the device." : firmware);
    set_text(ui.maintenance_labels[0], !ui.maintenance_action ? "Restart" : reset ? "Confirm reset" : "Confirm restart");
    set_text(ui.maintenance_labels[1], ui.maintenance_action ? "Cancel" : "Factory reset");
    set_text(ui.maintenance_labels[2], "Back");
    hidden(ui.maintenance_buttons[0], ui.maintenance_requested);
    hidden(ui.maintenance_buttons[1], ui.maintenance_requested);
}

static void on_maintenance(lv_event_t *e)
{
    const intptr_t button = (intptr_t)lv_event_get_user_data(e);
    if(button == 2) {
        ui.maintenance_visible = false;
        ui.maintenance_action = 0;
    } else if(!ui.maintenance_action) {
        ui.maintenance_action = button == 0 ? 1 : 2;
        ui.maintenance_failed = false;
        ui.maintenance_requested = false;
    } else if(button == 1) {
        ui.maintenance_action = 0;
        ui.maintenance_failed = false;
    } else if(!ui.maintenance_requested) {
        /* Only a new physical confirmation event reaches these callbacks. */
        const bool accepted = ui.maintenance_action == 2 ?
            (ui.callbacks.on_factory_reset && ui.callbacks.on_factory_reset(ui.callbacks.context)) :
            (ui.callbacks.on_restart && ui.callbacks.on_restart(ui.callbacks.context));
        ui.maintenance_requested = accepted;
        ui.maintenance_failed = !accepted;
    }
    refresh_settings();
}

static void refresh_settings(void)
{
    const palette_t *p = &ui.pal;
    const bool subview = ui.wifi_setup_visible || ui.web_login_visible || ui.maintenance_visible;
    hidden(ui.settings_nav, subview);
    hidden(ui.settings_list, subview);
    hidden(ui.settings_rule, subview);
    hidden(ui.wifi_setup, !ui.wifi_setup_visible);
    hidden(ui.maintenance, !ui.maintenance_visible);
    hidden(ui.web_login, !ui.web_login_visible);
    if(ui.web_login_visible) {
        poll_web_login();
        refresh_web_login();
        return;
    }
    if(ui.maintenance_visible) {
        refresh_maintenance();
        return;
    }
    if(ui.wifi_setup_visible) {
        poll_wifi_setup();
        refresh_wifi_setup();
        return;
    }
    for(int t = 0; t < TAB_COUNT; ++t) {
        const bool on = t == ui.tab;
        set_text(ui.tab_labels[t], TAB_NAMES[t] ? TAB_NAMES[t] : (ui.buddy_name[0] ? ui.buddy_name : "Arvel"));
        bg(ui.tabs[t], on ? (ui.dark ? lv_color_hex(0x8FC3BB) : lv_color_hex(0xEDF4F3)) : p->ink, on ? (ui.dark ? 46 : 255) : 0);
        ink(ui.tab_labels[t], on ? (ui.dark ? lv_color_hex(0xE3EFEC) : lv_color_hex(0x2F4F4B)) : p->ink);
        lv_obj_set_style_text_font(ui.tab_labels[t], on ? F13S : F13M, 0);
    }
    char value[64];
    for(int i = 0; i < ROWS_PER_TAB; ++i) {
        row_slot_t *r = &ui.rows[i];
        const row_id_t id = TAB_ROWS[ui.tab][i];
        hidden(r->toggle, true);
        hidden(r->stepper, true);
        hidden(r->seg, true);
        hidden(r->value, true);
        lv_obj_set_style_border_width(r->row, i == ROWS_PER_TAB - 1 ? 0 : 1, 0);
        ink(r->label, p->ink);
        lv_obj_set_style_opa(r->label, LV_OPA_COVER, 0);
        lv_obj_t *control = NULL;
        switch(id) {
            case R_VOICE: set_text(r->label, "Local voice"); set_seg(r, VOICE_OPTS, ui.prefs.voice_style); control = r->seg; break;
            case R_SPEED:
                set_text(r->label, "Local speech speed");
                snprintf(value, sizeof(value), "%d.%d×", ui.prefs.speech_rate / 10, ui.prefs.speech_rate % 10);
                control = r->stepper; break;
            case R_HEALTH:
                set_text(r->label, "Native health (not audio)");
                set_value(r, ui.native_readiness[0] ? ui.native_readiness : "Not checked");
                control = r->value; break;
            case R_CAPTIONS: set_text(r->label, "Live captions"); set_toggle(r, ui.prefs.captions, true); control = r->toggle; break;
            case R_BRIGHT:
                set_text(r->label, "Brightness");
                snprintf(value, sizeof(value), "%d%%", ui.prefs.brightness);
                control = r->stepper; break;
            case R_SCENE: set_text(r->label, "Scene"); set_value(r, scene_name()); control = r->value; break;
            case R_DIM: set_text(r->label, "Dim at night"); set_toggle(r, ui.prefs.dim_at_night, true); control = r->toggle; break;
            case R_SLEEP: set_text(r->label, "Screen sleep"); set_seg(r, SLEEP_OPTS, ui.prefs.screen_sleep); control = r->seg; break;
            case R_PERSONA: set_text(r->label, "Personality"); set_seg(r, PERSONA_OPTS, ui.prefs.persona); control = r->seg; break;
            case R_MODEL: set_text(r->label, "Model"); set_value(r, ui.model[0] ? ui.model : "Not set"); control = r->value; break;
            case R_ALOUD: set_text(r->label, "Read replies aloud"); set_toggle(r, ui.prefs.read_aloud, true); control = r->toggle; break;
            case R_MEMORY:
                snprintf(value, sizeof(value), "%s · %.36s", ui.storage_state[0] ? ui.storage_state : "Storage unavailable",
                         ui.conversation_id[0] ? ui.conversation_id : "No archive");
                set_text(r->label, "Local memory / conversation");
                set_value(r, value); control = r->value; break;
            case R_VOLUME:
                set_text(r->label, "Volume");
                snprintf(value, sizeof(value), "%d%%", ui.prefs.volume);
                control = r->stepper; break;
            case R_WIFI: set_text(r->label, "Wi-Fi setup"); set_value(r, "Tap to open local setup"); control = r->value; break;
            case R_ACTIONS: set_text(r->label, "Device actions"); set_value(r, "Restart / Factory reset"); control = r->value; break;
            case R_WEB_LOGIN: set_text(r->label, "Web login"); set_value(r, "Create / Recover login"); control = r->value; break;
        }
        if(control == r->stepper) set_text(r->step_value, value);
        hidden(control, false);
    }
}

/* --------------------------------------------------------------- tool pages */

static bool page_mode(ui_mode_t mode)
{
    return mode >= MODE_TIMER;
}

static bool timer_active(void)
{
    return ui.timer_known && (ui.timer.status == BUDDY_UI_TIMER_RUNNING || ui.timer.status == BUDDY_UI_TIMER_PAUSED);
}

/* An expiry the screen has not shown yet. */
static bool timer_alert(void)
{
    return ui.timer_known && ui.timer.status == BUDDY_UI_TIMER_DONE && ui.timer.notification_id &&
           ui.timer.notification_id != ui.timer_seen_id;
}

/* Where the screen settles when nothing is happening: an unseen timer expiry,
 * the page of the tool the last reply used, or the idle greeting. */
static ui_mode_t rest_mode(void)
{
    if(timer_alert()) return MODE_TIMER_DONE;
    const ui_mode_t page = ui.reply_page;
    ui.reply_page = MODE_IDLE;
    if(page == MODE_TIMER && !timer_active()) return ui.timer_known ? MODE_TIMER_DONE : MODE_IDLE;
    return page;
}

static void page_notice(const char *notice)
{
    copy(ui.page_notice, sizeof(ui.page_notice), notice);
    ui.page_notice_tick = lv_tick_get();
    refresh_page();
}

/* Sends a page control to the desk tools; the result arrives through the setters. */
static bool desk(const char *tool, const char *arguments)
{
    if(ui.callbacks.desk_tool && ui.callbacks.desk_tool(tool, arguments, ui.callbacks.context)) {
        ui.desk_pending = true;
        return true;
    }
    page_notice("Busy — try again in a moment");
    return false;
}

/* "1,234.5", with at most `decimals` places and no trailing zeros; "−" for negatives. */
static void format_number(char *out, size_t size, double value, int decimals)
{
    char digits[48];
    if(value != value || value > 1e15 || value < -1e15) {
        snprintf(out, size, "%.4g", value);
        return;
    }
    snprintf(digits, sizeof(digits), "%.*f", decimals, value < 0 ? -value : value);
    char *dot = strchr(digits, '.');
    if(dot) {
        char *end = digits + strlen(digits) - 1;
        while(end > dot && *end == '0') *end-- = '\0';
        if(end == dot) *end = '\0';
    }
    const size_t whole = dot && *dot ? (size_t)(dot - digits) : strlen(digits);
    const bool zero = strspn(digits, "0.") == strlen(digits);
    size_t n = 0;
    if(value < 0 && !zero && n + 3 < size) { memcpy(out, "\xE2\x88\x92", 3); n = 3; }
    for(size_t i = 0; digits[i] && n + 1 < size; ++i) {
        if(i && i < whole && (whole - i) % 3 == 0 && n + 2 < size) out[n++] = ',';
        out[n++] = digits[i];
    }
    out[n] = '\0';
}

/* Decimals that keep about `significant` digits of a value. */
static int decimals_for(double value, int significant)
{
    double magnitude = value < 0 ? -value : value;
    int digits = 1;
    while(magnitude >= 10) { magnitude /= 10; ++digits; }
    while(magnitude > 0 && magnitude < 1) { magnitude *= 10; --digits; }
    return LV_CLAMP(0, significant - digits, 6);
}

static int unit_index(const char *token)
{
    for(size_t i = 0; token && i < UNIT_COUNT; ++i) if(strcmp(UNITS[i].token, token) == 0) return (int)i;
    return -1;
}

static double convert_unit(double value, int from, int to)
{
    const double base = value * UNITS[from].scale + UNITS[from].offset;
    return (base - UNITS[to].offset) / UNITS[to].scale;
}

/* "18:40", or "1:05:00" from an hour up. */
static void format_clock(char *out, size_t size, uint32_t seconds)
{
    if(seconds >= 3600) snprintf(out, size, "%u:%02u:%02u", (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
    else snprintf(out, size, "%02u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
}

/* "25 min", "1 h 30 min", "45 s". */
static void format_duration(char *out, size_t size, uint32_t seconds)
{
    if(seconds < 60) snprintf(out, size, "%u s", (unsigned)seconds);
    else if(seconds < 3600 || seconds % 3600 == 0) {
        if(seconds < 3600) snprintf(out, size, "%u min", (unsigned)((seconds + 30) / 60));
        else snprintf(out, size, "%u h", (unsigned)(seconds / 3600));
    } else snprintf(out, size, "%u h %u min", (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60));
}

/* WMO weather code to the design's icon: sun, partly, cloud or rain. */
static uint8_t weather_kind(int code)
{
    if(code <= 1) return 0;
    if(code == 2) return 1;
    if(code == 3 || code == 45 || code == 48 || (code >= 71 && code <= 77) || code == 85 || code == 86) return 2;
    return 3;
}

static void set_weather_icon(lv_obj_t *obj, int code, int size)
{
    const uint8_t kind = weather_kind(code);
    set_icon_png(obj, &ui.weather_imgs[kind][size], &arvel_weather_icons[kind], size);
}

static void format_degrees(char *out, size_t size, int degrees)
{
    snprintf(out, size, degrees < 0 ? "\xE2\x88\x92%d°" : "%d°", degrees < 0 ? -degrees : degrees);
}

static void set_page_button(lv_obj_t *button, lv_obj_t *label, const char *value, bool primary)
{
    const palette_t *p = &ui.pal;
    set_text(label, value);
    lv_obj_set_style_text_font(label, primary ? F12T : F12S, 0);
    ink(label, primary ? p->active_ink : p->ink);
    bg(button, primary ? p->accent : p->chip, primary ? LV_OPA_COVER : p->chip_opa);
    lv_obj_set_style_pad_hor(button, primary ? 14 : 12, 0);
}

static void refresh_timer(char *title, size_t title_size, char *meta, size_t meta_size)
{
    const palette_t *p = &ui.pal;
    const buddy_ui_timer_t *t = &ui.timer;
    const bool active = timer_active();
    const bool pomodoro = active ? t->pomodoro : true;
    const bool on_break = active && t->pomodoro && t->on_break;
    const unsigned round = active && t->round ? t->round : 1, rounds = active && t->rounds ? t->rounds : 4;
    if(!pomodoro) copy(title, title_size, "TIMER");
    else if(on_break) copy(title, title_size, "BREAK");
    else snprintf(title, title_size, "FOCUS · ROUND %u OF %u", round, rounds);
    copy(meta, meta_size, !pomodoro ? "" : on_break ? "Pomodoro break" : "Pomodoro");

    /* Until a started timer reports in, show the 25-minute focus round it asked for. */
    const uint32_t total = active ? (t->duration_seconds ? t->duration_seconds : 1) : 1500;
    const uint32_t left = active ? LV_MIN(t->remaining_seconds, total) : 1500;
    char value[48];
    format_clock(value, sizeof(value), left);
    set_text(ui.timer_time, value);
    set_text(ui.timer_sub, !active ? "starting" : t->status == BUDDY_UI_TIMER_PAUSED ? "paused" : "left");
    lv_arc_set_value(ui.timer_ring, (int16_t)((uint64_t)left * 1000 / total));
    lv_obj_set_style_arc_color(ui.timer_ring, on_break ? p->accent : lv_color_hex(TIMER_RED), LV_PART_INDICATOR);
    set_text(ui.timer_label, on_break ? "Stretch, refill your water" : active && t->label && t->label[0] ? t->label : "Focus");

    for(unsigned i = 0; i < TIMER_DOTS; ++i) {
        hidden(ui.timer_dots[i], !pomodoro || i >= rounds);
        const bool past = i + 1 < round || (i + 1 == round && on_break);
        if(past) bg(ui.timer_dots[i], lv_color_hex(TIMER_RED), LV_OPA_COVER);
        else if(i + 1 == round) bg(ui.timer_dots[i], lv_color_hex(TIMER_RED), 115);
        else bg(ui.timer_dots[i], p->dot_off, p->dot_off_opa);
    }
    if(!pomodoro) {
        format_duration(value, sizeof(value), total);
        strncat(value, " timer", sizeof(value) - strlen(value) - 1);
    } else if(on_break) {
        if(round >= rounds) copy(value, sizeof(value), "Break · last round done");
        else snprintf(value, sizeof(value), "Break · round %u next", round + 1);
    } else snprintf(value, sizeof(value), "Round %u of %u", round, rounds);
    set_text(ui.timer_rounds, value);
    lv_obj_set_style_pad_left(ui.timer_rounds, pomodoro ? 4 : 0, 0);
    set_page_button(ui.timer_buttons[0], ui.timer_button_labels[0], t->status == BUDDY_UI_TIMER_PAUSED ? "Resume" : "Pause", true);
    set_page_button(ui.timer_buttons[1], ui.timer_button_labels[1], "+5 min", false);
    set_page_button(ui.timer_buttons[2], ui.timer_button_labels[2], "Cancel", false);
}

static void refresh_done(char *title, size_t title_size, char *meta, size_t meta_size)
{
    const buddy_ui_timer_t *t = &ui.timer;
    copy(title, title_size, "TIMER · DONE");
    const uint32_t minutes = lv_tick_elaps(ui.timer_done_tick) / 60000;
    if(minutes < 1) copy(meta, meta_size, "just now");
    else snprintf(meta, meta_size, "%u min ago", (unsigned)minutes);
    static char line[200];
    char button[32];
    const char *second = "Snooze 5 min";
    if(t->pomodoro && !t->on_break) {
        set_text(ui.done_title, "Focus round done.");
        set_text(ui.done_sub, "Nice work. Take five — I’ll ping you when the break is over.");
        copy(button, sizeof(button), "Start break");
        ui.done_actions[0] = "start_break";
    } else if(t->pomodoro && t->round < t->rounds) {
        set_text(ui.done_title, "Break’s over.");
        snprintf(line, sizeof(line), "Ready for round %u of %u? I’ll keep time while you focus.", t->round + 1u, (unsigned)t->rounds);
        set_text(ui.done_sub, line);
        snprintf(button, sizeof(button), "Start round %u", t->round + 1u);
        ui.done_actions[0] = "start_next";
    } else {
        if(t->pomodoro) {
            set_text(ui.done_title, "All rounds done.");
            snprintf(line, sizeof(line), "%u focus rounds — that’s a good session.", (unsigned)t->rounds);
        } else {
            set_text(ui.done_title, "Time’s up.");
            char total[24];
            format_duration(total, sizeof(total), t->duration_seconds);
            if(t->label && t->label[0] && strcmp(t->label, "Timer") != 0) snprintf(line, sizeof(line), "%s — your %s timer is done.", t->label, total);
            else snprintf(line, sizeof(line), "Your %s timer is done.", total);
        }
        set_text(ui.done_sub, line);
        copy(button, sizeof(button), "Done");
        ui.done_actions[0] = "cancel";
    }
    ui.done_actions[1] = "snooze";
    set_page_button(ui.done_buttons[0], ui.done_button_labels[0], button, true);
    set_page_button(ui.done_buttons[1], ui.done_button_labels[1], second, false);
}

/* To-dos first, in their saved order, then notes. */
static size_t note_order(size_t *order)
{
    size_t n = 0;
    for(int pass = 0; pass < 2; ++pass) {
        for(size_t i = 0; i < ui.note_count; ++i) if(ui.notes[i].task == (pass == 0)) order[n++] = i;
    }
    return n;
}

static void build_note_view(note_view_t *v);
static void on_note(lv_event_t *e);

static void refresh_notes(char *title, size_t title_size, char *meta, size_t meta_size)
{
    const palette_t *p = &ui.pal;
    size_t order[NOTE_ROWS], open = 0, tasks = 0;
    const size_t count = note_order(order);
    for(size_t i = 0; i < ui.note_count; ++i) {
        tasks += ui.notes[i].task;
        open += ui.notes[i].task && !ui.notes[i].completed;
    }
    if(tasks || !count) snprintf(title, title_size, ui.notes_ready && tasks ? "TO-DO · %u OPEN" : "TO-DO", (unsigned)open);
    else snprintf(title, title_size, "NOTES · %u SAVED", (unsigned)count);
    copy(meta, meta_size, "say “add …” to add");

    hidden(ui.notes_empty, ui.notes_ready && count);
    set_text(ui.notes_empty, !ui.notes_ready ? "I can’t read your saved notes right now." :
             "Nothing on your list yet. Say “add …” and I’ll keep it here.");
    while(ui.note_view_count < count) build_note_view(&ui.note_views[ui.note_view_count++]);
    for(size_t r = 0; r < ui.note_view_count; ++r) {
        note_view_t *v = &ui.note_views[r];
        const note_slot_t *n = r < count ? &ui.notes[order[r]] : NULL;
        hidden(v->task, !n || !n->task);
        hidden(v->note, !n || n->task);
        if(!n) continue;
        lv_obj_set_user_data(v->task, (void *)(uintptr_t)n->id);
        if(!n->task) {
            set_text(v->note_label, n->text);
            ink(v->note_label, p->ink);
            bg(lv_obj_get_child(v->note, 0), p->note_bg, p->note_bg_opa);
            continue;
        }
        set_text(v->label, n->text);
        ink(v->label, n->completed ? p->faint : p->ink);
        lv_obj_set_style_text_decor(v->label, n->completed ? LV_TEXT_DECOR_STRIKETHROUGH : LV_TEXT_DECOR_NONE, 0);
        lv_obj_set_style_border_color(v->box, n->completed ? p->accent : p->box_border, 0);
        bg(v->box, p->accent, n->completed ? LV_OPA_COVER : LV_OPA_TRANSP);
        hidden(v->check, !n->completed);
        const char *tag = n->completed ? "Done" : n->due;
        hidden(v->tag, !tag[0]);
        set_text(v->tag, tag);
        if(n->completed) {
            bg(v->tag, p->chip, LV_OPA_TRANSP);
            ink(v->tag, p->faint);
        } else if(n->urgent) {
            bg(v->tag, p->urgent_bg, p->urgent_opa);
            ink(v->tag, p->urgent_ink);
        } else {
            bg(v->tag, p->chip, p->chip_opa);
            ink(v->tag, p->chip_ink);
        }
    }
}

static void refresh_calc(char *title, size_t title_size, char *meta, size_t meta_size)
{
    const palette_t *p = &ui.pal;
    copy(title, title_size, "CALCULATOR");
    copy(meta, meta_size, "on device · no cloud");
    hidden(ui.calc_empty, ui.calc_known);
    hidden(ui.calc_expr, !ui.calc_known);
    hidden(ui.calc_value_row, !ui.calc_known);
    const bool units = ui.calc_known && ui.calc_conversion && ui.calc_from >= 0 && ui.calc_to >= 0;
    hidden(ui.calc_bottom, !units);
    hidden(ui.calc_unit, !units);
    if(!ui.calc_known) return;

    /* The expression as typed, with the design's × and ÷. */
    static char expression[300], line[360];
    char number[48];
    size_t n = 0;
    for(const char *c = ui.calc_expression; *c && n + 4 < sizeof(expression); ++c) {
        if(*c == '*' || *c == '/') {
            const char *sign = *c == '*' ? "×" : "÷";
            if(n && expression[n - 1] != ' ') expression[n++] = ' ';
            memcpy(expression + n, sign, strlen(sign));
            n += strlen(sign);
            if(c[1] != ' ') expression[n++] = ' ';
        } else expression[n++] = *c;
    }
    expression[n] = '\0';
    const int to = units ? (ui.calc_shown >= 0 ? ui.calc_shown : ui.calc_to) : -1;
    if(units) {
        if(!expression[0]) format_number(expression, sizeof(expression), ui.calc_input, decimals_for(ui.calc_input, 6));
        snprintf(line, sizeof(line), "%s %s → %s", expression, UNITS[ui.calc_from].name, UNITS[to].name);
    } else snprintf(line, sizeof(line), "%s =", expression);
    set_text(ui.calc_expr, line);

    const double result = units ? (to == ui.calc_to ? ui.calc_result : convert_unit(ui.calc_input, ui.calc_from, to)) : ui.calc_result;
    double magnitude = result < 0 ? -result : result;
    format_number(number, sizeof(number), result, magnitude >= 1 || magnitude == 0 ? 2 : decimals_for(result, 3));
    set_text(ui.calc_value, number);
    if(!units) return;
    set_text(ui.calc_unit, UNITS[to].name);
    for(int i = 0; i < 3; ++i) {
        const int u = ui.calc_choice[i];
        hidden(ui.calc_opts[i], u < 0);
        if(u < 0) continue;
        const bool on = u == to;
        set_text(ui.calc_opt_labels[i], UNITS[u].name);
        lv_obj_set_style_text_font(ui.calc_opt_labels[i], on ? F12T : F12M, 0);
        ink(ui.calc_opt_labels[i], p->ink);
        bg(ui.calc_opts[i], p->seg_on, on ? LV_OPA_COVER : LV_OPA_TRANSP);
        lv_obj_set_style_shadow_opa(ui.calc_opts[i], on ? 31 : 0, 0);
    }
    /* "1 kg = 2.2046 lb": the rate is only constant for scaled units. */
    hidden(ui.calc_rate, UNITS[to].dimension == TEMPERATURE);
    const double rate = convert_unit(1, ui.calc_from, to);
    format_number(number, sizeof(number), rate, decimals_for(rate, 5));
    snprintf(line, sizeof(line), "1 %s = %s %s", UNITS[ui.calc_from].name, number, UNITS[to].name);
    set_text(ui.calc_rate, line);
}

static void refresh_weather(char *title, size_t title_size, char *meta, size_t meta_size)
{
    copy(title, title_size, "WEATHER");
    hidden(ui.weather_empty, ui.weather_known);
    hidden(ui.weather_main, !ui.weather_known);
    hidden(ui.weather_footer, !ui.weather_known);
    if(!ui.weather_known) {
        meta[0] = '\0';
        return;
    }
    snprintf(meta, meta_size, ui.weather_observed[0] ? "%s · %s" : "%s", ui.weather_location, ui.weather_observed);
    static char value[160];
    char part[48];
    set_weather_icon(ui.weather_icon, ui.weather_code, 0);
    format_degrees(value, sizeof(value), ui.weather_degrees);
    set_text(ui.weather_temp, value);
    set_text(ui.weather_condition, ui.weather_condition_text);
    const char *rain_from = NULL;
    uint8_t rain = 0;
    for(size_t i = 0; i < DESK_HOURS; ++i) {
        const bool shown = i < ui.weather_hours;
        hidden(lv_obj_get_parent(ui.weather_hour_time[i]), !shown);
        if(!shown) continue;
        set_text(ui.weather_hour_time[i], ui.weather_forecast[i].time);
        set_weather_icon(ui.weather_hour_icon[i], ui.weather_forecast[i].code, 1);
        format_degrees(part, sizeof(part), ui.weather_forecast[i].temp);
        set_text(ui.weather_hour_temp[i], part);
        if(!rain_from && ui.weather_forecast[i].rain >= 50) rain_from = ui.weather_forecast[i].time;
        rain = LV_MAX(rain, ui.weather_forecast[i].rain);
    }
    char high[16], low[16], feels[16];
    format_degrees(high, sizeof(high), ui.weather_high);
    format_degrees(low, sizeof(low), ui.weather_low);
    format_degrees(feels, sizeof(feels), ui.weather_feels);
    snprintf(value, sizeof(value), "H %s · L %s · feels like %s", high, low, feels);
    if(rain_from) snprintf(part, sizeof(part), " · rain likely from %s", rain_from);
    else if(rain >= 20) snprintf(part, sizeof(part), " · %u%% chance of rain", (unsigned)rain);
    else part[0] = '\0';
    strncat(value, part, sizeof(value) - strlen(value) - 1);
    set_text(ui.weather_footer, value);
}

/* The page's header and the box for the current page. */
static void refresh_page(void)
{
    if(!ui.page_box || !page_mode(ui.mode)) return;
    static const uint8_t KINDS[] = {BUDDY_TOOL_TIMER, BUDDY_TOOL_TIMER, BUDDY_TOOL_NOTES, BUDDY_TOOL_CALC, BUDDY_TOOL_WEATHER};
    set_tool_icon(ui.page_icon, KINDS[ui.mode - MODE_TIMER], 0);
    lv_obj_t *const boxes[] = {ui.timer_box, ui.done_box, ui.notes_box, ui.calc_box, ui.weather_box};
    for(size_t i = 0; i < 5; ++i) if(boxes[i]) hidden(boxes[i], ui.mode != MODE_TIMER + (int)i);
    /* Static: the LVGL task's stack is small; pages only refresh on that task. */
    static char title[48], meta[160];
    switch(ui.mode) {
        case MODE_TIMER: refresh_timer(title, sizeof(title), meta, sizeof(meta)); break;
        case MODE_TIMER_DONE: refresh_done(title, sizeof(title), meta, sizeof(meta)); break;
        case MODE_NOTES: refresh_notes(title, sizeof(title), meta, sizeof(meta)); break;
        case MODE_CALC: refresh_calc(title, sizeof(title), meta, sizeof(meta)); break;
        default: refresh_weather(title, sizeof(title), meta, sizeof(meta)); break;
    }
    if(ui.page_notice[0] && lv_tick_elaps(ui.page_notice_tick) < PAGE_NOTICE_MS) copy(meta, sizeof(meta), ui.page_notice);
    else ui.page_notice[0] = '\0';
    set_text(ui.page_title, title);
    /* icon, title, spacer, meta and close share the row with four 7px gaps. */
    const int title_w = text_width(title, F11B, 1);
    set_fitted(ui.page_meta, meta, F11M, 1, LV_MAX(0, 364 - 22 - 30 - 4 * 7 - title_w));
}

static void apply_page_colors(void)
{
    const palette_t *p = &ui.pal;
    if(!ui.page_box) return;
    ink(ui.page_title, p->sub);
    ink(ui.page_meta, p->sub);
    bg(ui.page_close, p->chip, p->chip_opa);
    tint(lv_obj_get_child(ui.page_close, 0), p->ink);
    if(ui.timer_box) {
        lv_obj_set_style_arc_color(ui.timer_ring, p->ring_track, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(ui.timer_ring, p->ring_track_opa, LV_PART_MAIN);
        ink(ui.timer_time, p->ink);
        ink(ui.timer_sub, p->sub);
        ink(ui.timer_label, p->ink);
        ink(ui.timer_rounds, p->sub);
    }
    if(ui.done_box) {
        ink(ui.done_title, p->ink);
        ink(ui.done_sub, p->sub);
    }
    if(ui.notes_box) ink(ui.notes_empty, p->sub);
    if(ui.calc_box) {
        ink(ui.calc_expr, p->sub);
        ink(ui.calc_value, p->ink);
        ink(ui.calc_unit, p->sub);
        bg(ui.calc_seg, p->chip, p->chip_opa);
        ink(ui.calc_rate, p->sub);
        ink(ui.calc_empty, p->sub);
    }
    if(ui.weather_box) {
        ink(ui.weather_temp, p->ink);
        ink(ui.weather_condition, p->sub);
        bg(lv_obj_get_child(ui.weather_main, 1), p->rule, p->rule_opa);
        for(size_t i = 0; i < DESK_HOURS; ++i) {
            ink(ui.weather_hour_time[i], p->sub);
            ink(ui.weather_hour_temp[i], p->ink);
        }
        ink(ui.weather_footer, p->sub);
        ink(ui.weather_empty, p->sub);
    }
    refresh_page();
}

/* Tool page controls. */

static void on_suggest(lv_event_t *e)
{
    ui.notice[0] = '\0';
    switch(SUGGEST[(intptr_t)lv_event_get_user_data(e)].kind) {
        case BUDDY_TOOL_TIMER:
            if(ui.timer_known && ui.timer.status == BUDDY_UI_TIMER_DONE) { set_mode(MODE_TIMER_DONE); return; }
            if(!timer_active() &&
               !desk("manage_timer", "{\"action\":\"start\",\"kind\":\"pomodoro\",\"focus_seconds\":1500,\"label\":\"Focus\"}")) {
                return;
            }
            set_mode(MODE_TIMER);
            break;
        case BUDDY_TOOL_NOTES: set_mode(MODE_NOTES); break;
        case BUDDY_TOOL_CALC: set_mode(MODE_CALC); break;
        default: set_mode(MODE_WEATHER); break;
    }
}

static void on_page_close(lv_event_t *e)
{
    (void)e;
    ui.reply_page = MODE_IDLE;
    set_mode(MODE_IDLE);
}

static void on_timer_button(lv_event_t *e)
{
    char arguments[48];
    switch((intptr_t)lv_event_get_user_data(e)) {
        case 0:
            if(!timer_active()) return;
            snprintf(arguments, sizeof(arguments), "{\"action\":\"%s\"}", ui.timer.status == BUDDY_UI_TIMER_PAUSED ? "resume" : "pause");
            desk("manage_timer", arguments);
            break;
        case 1:
            if(timer_active()) desk("manage_timer", "{\"action\":\"extend\",\"seconds\":300}");
            break;
        default:
            if(timer_active() && !desk("manage_timer", "{\"action\":\"cancel\"}")) return;
            ui.timer_known = false;
            set_mode(MODE_IDLE);
            break;
    }
}

static void on_done_button(lv_event_t *e)
{
    const char *action = ui.done_actions[(intptr_t)lv_event_get_user_data(e)];
    if(!action) return;
    char arguments[64];
    snprintf(arguments, sizeof(arguments), strcmp(action, "snooze") == 0 ? "{\"action\":\"%s\",\"seconds\":300}" : "{\"action\":\"%s\"}", action);
    if(!desk("manage_timer", arguments)) return;
    if(strcmp(action, "cancel") == 0) {
        ui.timer_known = false;
        set_mode(MODE_IDLE);
    } else {
        /* Running again: the timer page shows the new round until the backend reports it. */
        ui.timer.status = BUDDY_UI_TIMER_RUNNING;
        ui.timer.remaining_seconds = ui.timer.duration_seconds;
        if(strcmp(action, "start_break") == 0) ui.timer.on_break = true;
        else if(strcmp(action, "start_next") == 0) { ui.timer.on_break = false; ++ui.timer.round; }
        set_mode(MODE_TIMER);
    }
}

static void on_note(lv_event_t *e)
{
    const uint32_t id = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    for(size_t i = 0; i < ui.note_count; ++i) {
        note_slot_t *n = &ui.notes[i];
        if(n->id != id || !n->task) continue;
        char arguments[80];
        snprintf(arguments, sizeof(arguments), "{\"action\":\"update\",\"id\":%u,\"completed\":%s}",
                 (unsigned)id, n->completed ? "false" : "true");
        /* Ticked straight away; the next notes snapshot confirms or restores it. */
        if(desk("manage_notes", arguments)) n->completed = !n->completed;
        refresh_page();
        return;
    }
}

static void on_calc_unit(lv_event_t *e)
{
    const int u = ui.calc_choice[(intptr_t)lv_event_get_user_data(e)];
    if(u < 0) return;
    ui.calc_shown = (int8_t)u;
    refresh_page();
}

/* -------------------------------------------------------------------- modes */

static void group(track_group_t g, bool on)
{
    ui.group_on[g] = on;
}

static void set_mode(ui_mode_t mode)
{
    const ui_mode_t previous = ui.mode;
    const palette_t *p = &ui.pal;
    ui.mode = mode;
    if(mode != MODE_SETTINGS) {
        ui.maintenance_visible = false;
        ui.maintenance_action = 0;
        ui.maintenance_requested = false;
        ui.maintenance_failed = false;
    }
    if(!page_mode(mode)) ui.page_pinned = false;
    ensure_page(mode);
    const bool panel = mode == MODE_SCENES || mode == MODE_SETTINGS;
    if(previous == MODE_SCENES && mode != MODE_SCENES) free_scene_thumbs();
    hidden(ui.bird_box, panel);
    hidden(ui.bubble, panel);
    hidden(ui.tail, panel);
    hidden(ui.pill, panel);
    const bool live = ui.voice_active && !panel && mode != MODE_CHAT && !page_mode(mode);
    hidden(ui.ripple_box, mode != MODE_LISTENING || live);
    hidden(ui.scenes_panel, mode != MODE_SCENES);
    hidden(ui.settings_panel, mode != MODE_SETTINGS);
    const bool page = page_mode(mode) && !live;
    hidden(ui.status_row, mode == MODE_CHAT || page);
    if(ui.page_box) hidden(ui.page_box, !page);
    hidden(ui.idle_box, mode != MODE_IDLE || live);
    hidden(ui.listen_box, mode != MODE_LISTENING || live);
    hidden(ui.think_box, mode != MODE_THINKING || live);
    hidden(ui.speak_box, mode != MODE_SPEAKING || live);
    hidden(ui.live_box, !live);
    hidden(ui.chat_box, mode != MODE_CHAT);
    const bool pending_stop = !ui.voice_active &&
        (ui.state == BUDDY_CLOCK_SYNC || ui.state == BUDDY_TRANSCRIBING || ui.state == BUDDY_THINKING);
    const bool show_stop = pending_stop || (mode == MODE_LISTENING && !ui.voice_live_available);
    hidden(ui.mic_icon, show_stop);
    hidden(ui.stop_icon, !show_stop);
    hidden(ui.mute_slash, !ui.voice_active || !ui.voice_muted);
    bg(ui.mic_btn, ui.voice_active && ui.voice_muted ? p->rail_ink : lv_color_hex(MIC_RED), LV_OPA_COVER);
    const bool can_end = ui.busy || ui.voice_active || ui.voice_connecting ||
        ui.state == BUDDY_RECORDING || ui.state == BUDDY_PREVIEW || ui.state == BUDDY_SPEAKING;
    hidden(ui.end_btn, !can_end);
    hidden(ui.scene_btn, can_end);
    group(G_BIRD, !panel);
    group(G_LISTEN, mode == MODE_LISTENING && !live);
    group(G_THINK, mode == MODE_THINKING && !live);
    group(G_SPEAK, mode == MODE_SPEAKING && !live);
    group(G_BELL, mode == MODE_TIMER_DONE && !live);
    /* Arvel bobs faster while listening, speaking and ringing a finished timer. */
    const uint16_t bob = mode == MODE_SPEAKING || mode == MODE_TIMER_DONE ? 550 : mode == MODE_LISTENING ? 1600 : 3200;
    for(size_t i = 0; i < ui.track_count; ++i) {
        if(ui.tracks[i].values == BOB) ui.tracks[i].period = bob;
    }
    bg(ui.scene_btn, mode == MODE_SCENES ? p->accent : p->rail, mode == MODE_SCENES ? LV_OPA_COVER : p->rail_opa);
    tint(ui.scene_icon, mode == MODE_SCENES ? p->active_ink : p->rail_ink);
    bg(ui.chat_btn, mode == MODE_CHAT ? p->accent : p->rail, mode == MODE_CHAT ? LV_OPA_COVER : p->rail_opa);
    tint(ui.chat_icon, mode == MODE_CHAT ? p->active_ink : p->rail_ink);
    if(mode != previous) {
        if(mode == MODE_CHAT) { ui.chat_page = -1; refresh_chat(); }
        if(mode == MODE_SCENES) { ui.scene_page = (int)(ui.scene / SCENES_PER_PAGE); refresh_scenes(); }
        if(mode == MODE_SETTINGS) refresh_settings();
        if(mode == MODE_CALC) ui.calc_shown = -1;
        if(mode == MODE_NOTES) lv_obj_scroll_to_y(ui.notes_box, 0, LV_ANIM_OFF);
    }
    if(mode == MODE_TIMER_DONE && timer_alert()) {
        /* Shown: the backend stops announcing this expiry. */
        ui.timer_seen_id = ui.timer.notification_id;
        ui.timer_done_tick = lv_tick_get();
        if(ui.callbacks.timer_seen) ui.callbacks.timer_seen(ui.timer_seen_id, ui.callbacks.context);
    }
    refresh_page();
    refresh_status();
    if(ui.voice_active) refresh_live();
}

/* The activity mode the backend state asks for. */
static ui_mode_t activity_mode(void)
{
    switch(ui.state) {
        case BUDDY_RECORDING:
        case BUDDY_LISTENING:
        case BUDDY_PREVIEW: return MODE_LISTENING;
        case BUDDY_CLOCK_SYNC:
        case BUDDY_TRANSCRIBING:
        case BUDDY_VOICE_CONNECTING:
        case BUDDY_THINKING: return MODE_THINKING;
        case BUDDY_SPEAKING: return MODE_SPEAKING;
        case BUDDY_MUTED: return MODE_IDLE;
        default: return ui.replying ? MODE_SPEAKING : MODE_IDLE;
    }
}

static void linger_done(lv_timer_t *timer)
{
    (void)timer;
    ui.linger_timer = NULL;
    ui.replying = false;
    if(ui.mode == MODE_SPEAKING && ui.state != BUDDY_SPEAKING) set_mode(rest_mode());
}

static void end_reply_after(uint32_t ms)
{
    if(ui.linger_timer) lv_timer_del(ui.linger_timer);
    ui.linger_timer = lv_timer_create(linger_done, ms, NULL);
    lv_timer_set_repeat_count(ui.linger_timer, 1);
}

static void start_reply(bool spoken)
{
    ui.replying = true;
    ui.reply_spoken = spoken;
    ui.fresh_reply = false;
    ui.speak_started = lv_tick_get();
    ui.speak_words_shown = 1;
    if(ui.linger_timer) { lv_timer_del(ui.linger_timer); ui.linger_timer = NULL; }
    if(!spoken) end_reply_after(SILENT_REPLY_MS);
    set_mode(MODE_SPEAKING);
    refresh_speaking(!spoken);
}

/* ------------------------------------------------------------------ saving */

static void save_now(lv_timer_t *timer)
{
    (void)timer;
    if(!ui.callbacks.save) { ui.dirty = false; return; }
    const buddy_ui_save_result_t result = ui.callbacks.save(&ui.prefs, arvel_scenes[ui.scene].id, ui.callbacks.context);
    if(result == BUDDY_UI_SAVE_BUSY) {
        lv_timer_set_period(ui.save_timer, SAVE_RETRY_MS);
        lv_timer_reset(ui.save_timer);
        lv_timer_resume(ui.save_timer);
        return;
    }
    /* Saved or rejected: stop retrying and accept the device's settings again. */
    ui.dirty = false;
    lv_timer_pause(ui.save_timer);
}

static void prefs_changed(void)
{
    ui.dirty = true;
    if(ui.callbacks.apply_prefs) ui.callbacks.apply_prefs(&ui.prefs, ui.callbacks.context);
    lv_timer_set_period(ui.save_timer, SAVE_DELAY_MS);
    lv_timer_reset(ui.save_timer);
    lv_timer_resume(ui.save_timer);
    refresh_settings();
}

/* ------------------------------------------------------------------- scenes */

static void unload_scene(void)
{
    for(size_t i = 0; i < ui.scene_layer_count; ++i) {
        lv_obj_del(ui.scene_objs[i]);
        lv_mem_free(ui.scene_imgs[i]);
        ui.scene_objs[i] = NULL;
        ui.scene_imgs[i] = NULL;
    }
    ui.scene_layer_count = 0;
    ui.track_count = ui.scene_track_start;
}

static void load_scene(size_t index)
{
    if(index >= ARVEL_SCENE_COUNT || (ui.scene_loaded && index == ui.scene)) return;
    const arvel_scene_t *scene = &arvel_scenes[index];
    lv_img_dsc_t *images[MAX_LAYERS] = {0};
    const size_t count = LV_MIN(scene->layer_count, MAX_LAYERS);
    for(size_t i = 0; i < count; ++i) {
        images[i] = decode_png(scene->layers[i].png, scene->layers[i].png_size, i != 0);
        if(!images[i]) {
            /* Out of memory: keep the current scene rather than a partial one. */
            for(size_t j = 0; j < i; ++j) lv_mem_free(images[j]);
            return;
        }
    }
    unload_scene();
    ui.scene = index;
    ui.scene_loaded = true;
    for(size_t i = 0; i < count; ++i) {
        const arvel_layer_t *layer = &scene->layers[i];
        lv_obj_t *obj = lv_img_create(ui.scene_box);
        lv_img_set_src(obj, images[i]);
        lv_obj_set_pos(obj, layer->x, layer->y);
        ui.scene_objs[i] = obj;
        ui.scene_imgs[i] = images[i];
        for(uint8_t t = 0; t < layer->track_count; ++t) {
            const arvel_track_t *src = &layer->tracks[t];
            const track_prop_t prop = src->kind == ARVEL_TRACK_OPA ? P_OPA : src->kind == ARVEL_TRACK_DX ? P_X : P_Y;
            track_t *track = add_track(obj, G_SCENE, prop, src->values, src->count, src->period_ms, src->begin_ms);
            if(track) { track->base_x = layer->x; track->base_y = layer->y; }
        }
    }
    ui.scene_layer_count = count;
    ui.dark = scene->dark;
    make_palette(ui.dark);
    apply_colors();
}

/* ------------------------------------------------------------------- events */

static void toggle_mode(ui_mode_t target)
{
    ui.notice[0] = '\0';
    set_mode(ui.mode == target ? activity_mode() == target ? MODE_IDLE : activity_mode() : target);
}

static void on_settings(lv_event_t *e) { (void)e; toggle_mode(MODE_SETTINGS); }
static void on_scenes(lv_event_t *e) { (void)e; toggle_mode(MODE_SCENES); }
static void on_chat(lv_event_t *e) { (void)e; toggle_mode(MODE_CHAT); }

static void on_end(lv_event_t *e)
{
    (void)e;
    if(!ui.callbacks.voice_stop || !ui.callbacks.voice_stop(ui.callbacks.context)) {
        copy(ui.notice, sizeof(ui.notice), "Stop was not accepted. Tap End to retry.");
        set_text(ui.live_hint, ui.notice);
    }
}

static void on_mic(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED) { ui.mic_hold_handled = false; return; }
    if(code == LV_EVENT_LONG_PRESSED) {
        const bool pending = ui.state == BUDDY_CLOCK_SYNC || ui.state == BUDDY_TRANSCRIBING || ui.state == BUDDY_THINKING;
        if((!ui.voice_live_available || !ui.voice_active) && !pending) return;
        ui.mic_hold_handled = true;
        if(!ui.callbacks.voice_stop || !ui.callbacks.voice_stop(ui.callbacks.context)) {
            copy(ui.notice, sizeof(ui.notice), "Couldn’t stop. Hold Stop to try again.");
            if(ui.voice_active) set_text(ui.live_hint, ui.notice);
        } else if(ui.maintenance_requested) {
            ui.maintenance_requested = false;
            ui.maintenance_action = 0;
            refresh_settings();
        }
        return;
    }
    if(ui.mic_hold_handled) return;
    ui.notice[0] = '\0';
    if(!ui.voice_active && (ui.state == BUDDY_CLOCK_SYNC || ui.state == BUDDY_TRANSCRIBING || ui.state == BUDDY_THINKING)) {
        if(ui.callbacks.voice_stop && ui.callbacks.voice_stop(ui.callbacks.context) && ui.maintenance_requested) {
            ui.maintenance_requested = false;
            ui.maintenance_action = 0;
            refresh_settings();
        }
        return;
    }
    if(!ui.voice_live_available && (!ui.voice_configured || !ui.chat_configured)) {
        copy(ui.notice, sizeof(ui.notice), !ui.chat_configured ?
             "Open browser Settings to choose a conversation model and configure its credentials." :
             "Open browser Settings to configure the selected conversation transport.");
        ui.replying = false;
        set_mode(MODE_IDLE);
        return;
    }
    if(ui.voice_live_available && !ui.connected && !ui.voice_active) {
        copy(ui.notice, sizeof(ui.notice), "I’m offline — open Settings > Device > Wi-Fi.");
        set_mode(MODE_IDLE);
        return;
    }
    if(!ui.callbacks.voice || !ui.callbacks.voice(ui.callbacks.context)) {
        if(ui.state == BUDDY_IDLE || ui.state == BUDDY_ERROR) {
            copy(ui.notice, sizeof(ui.notice), ui.connected ? "I couldn’t start listening. Try again in a moment." :
                 "I’m offline — open Settings > Device > Wi-Fi.");
            set_mode(MODE_IDLE);
        }
    }
}

static void on_chat_page(lv_event_t *e)
{
    int16_t pairs[MAX_MESSAGES][2];
    const int count = (int)chat_pairs(pairs);
    const int delta = (int)(intptr_t)lv_event_get_user_data(e);
    const int next = ui.chat_page + delta;
    if(next < 0 || next >= count) return;
    ui.chat_page = next;
    refresh_chat();
}

static void on_scene_page(lv_event_t *e)
{
    const int pages = (ARVEL_SCENE_COUNT + SCENES_PER_PAGE - 1) / SCENES_PER_PAGE;
    const int next = ui.scene_page + (int)(intptr_t)lv_event_get_user_data(e);
    if(next < 0 || next >= pages) return;
    ui.scene_page = next;
    refresh_scenes();
}

static void on_scene_pick(lv_event_t *e)
{
    const int index = ui.scene_page * SCENES_PER_PAGE + (int)(intptr_t)lv_event_get_user_data(e);
    if(index >= ARVEL_SCENE_COUNT) return;
    load_scene((size_t)index);
    set_mode(MODE_IDLE);
    prefs_changed();
}

static void on_tab(lv_event_t *e)
{
    ui.wifi_setup_visible = false;
    ui.web_login_visible = false;
    ui.maintenance_visible = false;
    ui.tab = (int)(intptr_t)lv_event_get_user_data(e);
    refresh_settings();
}

static void on_toggle(lv_event_t *e)
{
    switch(TAB_ROWS[ui.tab][(intptr_t)lv_event_get_user_data(e)]) {
        case R_CAPTIONS: ui.prefs.captions = !ui.prefs.captions; break;
        case R_DIM: ui.prefs.dim_at_night = !ui.prefs.dim_at_night; break;
        case R_ALOUD: ui.prefs.read_aloud = !ui.prefs.read_aloud; break;
        default: return;
    }
    prefs_changed();
}

static void on_step(lv_event_t *e)
{
    const intptr_t data = (intptr_t)lv_event_get_user_data(e);
    const int delta = data >= 8 ? 1 : -1;
    switch(TAB_ROWS[ui.tab][data % 8]) {
        case R_SPEED: ui.prefs.speech_rate = (uint8_t)LV_CLAMP(5, ui.prefs.speech_rate + delta, 20); break;
        case R_BRIGHT: ui.prefs.brightness = (uint8_t)LV_CLAMP(10, ui.prefs.brightness + 10 * delta, 100); break;
        case R_VOLUME: ui.prefs.volume = (uint8_t)LV_CLAMP(0, ui.prefs.volume + 10 * delta, 100); break;
        default: return;
    }
    prefs_changed();
}

static void on_seg(lv_event_t *e)
{
    const intptr_t data = (intptr_t)lv_event_get_user_data(e);
    const uint8_t option = (uint8_t)(data % 4);
    switch(TAB_ROWS[ui.tab][data / 4]) {
        case R_VOICE: ui.prefs.voice_style = option; break;
        case R_SLEEP: ui.prefs.screen_sleep = option; break;
        case R_PERSONA: ui.prefs.persona = option; break;
        default: return;
    }
    prefs_changed();
}

static void on_value(lv_event_t *e)
{
    const row_id_t id = TAB_ROWS[ui.tab][(intptr_t)lv_event_get_user_data(e)];
    if(id == R_SCENE) set_mode(MODE_SCENES);
    else if(id == R_WIFI) {
        ui.wifi_setup_visible = true;
        start_wifi_setup();
        refresh_settings();
    }
    else if(id == R_WEB_LOGIN) {
        ui.wifi_setup_visible = false;
        ui.maintenance_visible = false;
        ui.web_login_visible = true;
        poll_web_login();
        if(!ui.web_login_seconds) start_web_login();
        refresh_settings();
    }
    else if(id == R_ACTIONS) {
        ui.wifi_setup_visible = false;
        ui.maintenance_visible = true;
        ui.maintenance_action = 0;
        ui.maintenance_requested = false;
        ui.maintenance_failed = false;
        refresh_settings();
    }
}

static void on_wake(lv_event_t *e)
{
    (void)e;
    hidden(ui.wake_catcher, true);
}

/* --------------------------------------------------------------- per frame */

static void frame_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t now = lv_tick_get();
    if(now - ui.wifi_setup_checked >= 1000) {
        ui.wifi_setup_checked = now;
        poll_wifi_setup();
        poll_web_login();
        if(page_mode(ui.mode)) refresh_page();   /* "just now", notices */
    }
    /* Scene layers run at 15 fps; the character and indicators at 30 fps. */
    const bool scene_frame = (++ui.frame & 1) == 0;
    for(size_t i = 0; i < ui.track_count; ++i) {
        track_t *t = &ui.tracks[i];
        if(!ui.group_on[t->group] || (t->group == G_SCENE && !scene_frame)) continue;
        apply_track(t, now);
    }
    if(ui.group_on[G_BELL] && ui.bell_track.obj) apply_track(&ui.bell_track, now);
    if(ui.mode == MODE_THINKING && ui.live_tool_count) {
        const uint16_t angle = (uint16_t)(now % SPIN_MS * 360 / SPIN_MS);
        for(size_t i = 0; i < THINK_ROWS; ++i) lv_arc_set_rotation(ui.think_rows[i].spinner, angle);
    }
    if(!ui.voice_active && ui.mode == MODE_SPEAKING && ui.state == BUDDY_SPEAKING && ui.prefs.captions && ui.wpm > 0) {
        const size_t words = 1 + (size_t)(lv_tick_elaps(ui.speak_started) * (uint32_t)ui.wpm / 60000U);
        if(words != ui.speak_words_shown) {
            ui.speak_words_shown = words;
            refresh_speaking(false);
        }
    }
    if(!ui.wifi_setup_visible && !ui.web_login_visible && !ui.maintenance_visible &&
       (ui.mode == MODE_CHAT || ui.mode == MODE_SCENES || ui.mode == MODE_SETTINGS ||
        ui.mode == MODE_NOTES || ui.mode == MODE_CALC || ui.mode == MODE_WEATHER) &&
       lv_disp_get_inactive_time(NULL) > PANEL_TIMEOUT_MS) {
        set_mode(activity_mode());
    }
}

/* ------------------------------------------------------------------ builders */

static lv_obj_t *round_button(lv_obj_t *parent, int y, lv_event_cb_t callback)
{
    lv_obj_t *obj = tap_target(parent, callback, NULL);
    place(obj, 586, y, 44, 44);
    lv_obj_set_style_radius(obj, 22, 0);
    shadow(obj, 8, 2, 31, SHADOW_INK);
    return obj;
}

static lv_obj_t *pill_button(lv_obj_t *parent, const lv_img_dsc_t *src, lv_event_cb_t callback, intptr_t data)
{
    lv_obj_t *obj = tap_target(parent, callback, (void *)data);
    lv_obj_set_size(obj, 36, 28);
    lv_obj_set_style_radius(obj, 14, 0);
    lv_obj_set_ext_click_area(obj, 8);
    lv_obj_center(icon(obj, src));
    return obj;
}

static lv_obj_t *dots_row(lv_obj_t *parent, lv_obj_t **dots, size_t count)
{
    lv_obj_t *row = box(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 6);
    flex(row, LV_FLEX_FLOW_ROW, 4);
    for(size_t i = 0; i < count; ++i) {
        dots[i] = box(row);
        lv_obj_set_size(dots[i], 6, 6);
        lv_obj_set_style_radius(dots[i], 3, 0);
    }
    /* The design's 6px right margin: an empty item adds one more flex gap. */
    lv_obj_set_size(box(parent), 0, 1);
    return row;
}

/* A tool call: icon, verb and target; thinking rows add a spinner or check. */
static void build_tool_view(tool_view_t *v, lv_obj_t *parent, bool chip)
{
    v->row = box(parent);
    lv_obj_set_size(v->row, chip ? LV_SIZE_CONTENT : LV_PCT(100), 24);
    flex(v->row, LV_FLEX_FLOW_ROW, chip ? 5 : 7);
    lv_obj_set_flex_align(v->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if(chip) {
        lv_obj_set_style_pad_left(v->row, 2, 0);
        lv_obj_set_style_pad_right(v->row, 9, 0);
        lv_obj_set_style_radius(v->row, 12, 0);
    }
    v->icon = lv_img_create(v->row);
    lv_obj_set_size(v->icon, chip ? 20 : 22, chip ? 20 : 22);
    lv_obj_add_flag(v->icon, LV_OBJ_FLAG_EVENT_BUBBLE);
    v->verb = text(v->row, chip ? F11T : F12T, "");
    v->target = text(v->row, chip ? M11 : M12, "");
    lv_label_set_long_mode(v->target, LV_LABEL_LONG_DOT);
    if(chip) return;
    spacer(v->row);
    lv_obj_t *status = box(v->row);
    lv_obj_set_size(status, 14, 14);
    v->spinner = lv_arc_create(status);
    lv_obj_remove_style_all(v->spinner);
    lv_obj_clear_flag(v->spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(v->spinner, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(v->spinner, 12, 12);
    lv_obj_center(v->spinner);
    lv_arc_set_bg_angles(v->spinner, 0, 360);
    lv_arc_set_range(v->spinner, 0, 360);
    lv_arc_set_value(v->spinner, 270);
    lv_obj_set_style_arc_width(v->spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(v->spinner, true, LV_PART_INDICATOR);
    v->check = icon(status, &arvel_icon_check);
    lv_obj_center(v->check);
}

static void build_character(void)
{
    ui.ripple_box = box(ui.screen);
    place(ui.ripple_box, 0, 0, 180, 172);
    for(int i = 0; i < 2; ++i) {
        ui.ripples[i] = box(ui.ripple_box);
        lv_obj_set_style_radius(ui.ripples[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ui.ripples[i], 2, 0);
        lv_obj_set_style_border_color(ui.ripples[i], lv_color_hex(MIC_RED), 0);
        track_t *r = add_track(ui.ripples[i], G_LISTEN, P_RADIUS, RIPPLE_R, 2, 1600, (uint16_t)(800 * i));
        if(r) { r->base_x = 88; r->base_y = 96; }
        add_track(ui.ripples[i], G_LISTEN, P_OPA, RIPPLE_OPA, 2, 1600, (uint16_t)(800 * i));
    }

    ui.bird_box = box(ui.screen);
    place(ui.bird_box, arvel_character_png.x, arvel_character_png.y, 170, 170);
    ui.bird_img = decode_png(arvel_character_png.png, arvel_character_png.png_size, true);
    ui.blink_img = decode_png(arvel_blink_png.png, arvel_blink_png.png_size, true);
    if(ui.bird_img) lv_img_set_src(lv_img_create(ui.bird_box), ui.bird_img);
    if(ui.blink_img) {
        lv_obj_t *blink = lv_img_create(ui.bird_box);
        lv_img_set_src(blink, ui.blink_img);
        lv_obj_set_pos(blink, arvel_blink_png.x, arvel_blink_png.y);
        lv_obj_set_style_opa(blink, LV_OPA_TRANSP, 0);
        track_t *t = add_track(blink, G_BIRD, P_OPA, BLINK, 5, 4600, 0);
        if(t) t->key_times = BLINK_TIMES;
    }
    track_t *bob = add_track(ui.bird_box, G_BIRD, P_TRANSLATE_Y, BOB, 3, 3200, 0);
    if(bob) bob->divisor = 10;
}

static void build_pill(void)
{
    ui.pill = tap_target(ui.screen, on_settings, NULL);
    lv_obj_set_pos(ui.pill, 8, 8);
    lv_obj_set_size(ui.pill, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_pad_hor(ui.pill, 11, 0);
    lv_obj_set_style_radius(ui.pill, 16, 0);
    lv_obj_set_ext_click_area(ui.pill, 6);
    shadow(ui.pill, 8, 2, 26, SHADOW_INK);
    flex(ui.pill, LV_FLEX_FLOW_ROW, 7);
    lv_obj_set_flex_align(ui.pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.clock = text(ui.pill, F12S, "--:--");
    ui.wifi = icon(ui.pill, &arvel_icon_wifi);
    icon(ui.pill, &arvel_icon_settings);
}

/* A 30px pill button of a tool page; refresh sets its text and style. */
static lv_obj_t *page_button(lv_obj_t *parent, lv_event_cb_t callback, intptr_t data, lv_obj_t **label)
{
    lv_obj_t *obj = tap_target(parent, callback, (void *)data);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_radius(obj, 15, 0);
    lv_obj_set_ext_click_area(obj, 4);
    *label = text(obj, F12S, "");
    lv_obj_center(*label);
    return obj;
}

static lv_obj_t *row(lv_obj_t *parent, int gap)
{
    lv_obj_t *obj = box(parent);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex(obj, LV_FLEX_FLOW_ROW, gap);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return obj;
}

/* One page body filling the bubble under the header. */
static lv_obj_t *page_body(lv_flex_flow_t flow, int gap)
{
    lv_obj_t *obj = box(ui.page_box);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_flex_grow(obj, 1);
    flex(obj, flow, gap);
    if(flow == LV_FLEX_FLOW_ROW) lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return obj;
}

/* An alpha-mask icon as a coloured image, so it can be rotated. */
static lv_img_dsc_t *coloured(const lv_img_dsc_t *mask, lv_color_t color)
{
    const size_t px = (size_t)mask->header.w * mask->header.h;
    lv_img_dsc_t *img = psram_alloc(sizeof(lv_img_dsc_t) + px * LV_IMG_PX_SIZE_ALPHA_BYTE);
    if(!img) return NULL;
    uint8_t *data = (uint8_t *)(img + 1);
    for(size_t i = 0; i < px; ++i) {
        memcpy(data + i * LV_IMG_PX_SIZE_ALPHA_BYTE, &color, sizeof(lv_color_t));
        data[i * LV_IMG_PX_SIZE_ALPHA_BYTE + sizeof(lv_color_t)] = mask->data[i];
    }
    img->header = mask->header;
    img->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    img->data_size = (uint32_t)(px * LV_IMG_PX_SIZE_ALPHA_BYTE);
    img->data = data;
    return img;
}

static void build_note_view(note_view_t *v)
{
    /* To-do: 16px checkbox, text and due tag on a 23px row. */
    v->task = tap_target(ui.notes_box, on_note, NULL);
    lv_obj_set_size(v->task, LV_PCT(100), 23);
    flex(v->task, LV_FLEX_FLOW_ROW, 9);
    lv_obj_set_flex_align(v->task, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    v->box = box(v->task);
    lv_obj_set_size(v->box, 16, 16);
    lv_obj_set_style_radius(v->box, 5, 0);
    lv_obj_set_style_border_width(v->box, 2, 0);
    v->check = icon(v->box, &arvel_icon_check_small);
    tint(v->check, lv_color_white());
    lv_obj_center(v->check);
    v->label = text(v->task, F13M, "");
    lv_obj_set_width(v->label, 1);
    lv_obj_set_flex_grow(v->label, 1);
    lv_label_set_long_mode(v->label, LV_LABEL_LONG_DOT);
    v->tag = text(v->task, F10S, "");
    lv_obj_set_style_pad_ver(v->tag, 2, 0);
    lv_obj_set_style_pad_hor(v->tag, 7, 0);
    lv_obj_set_style_radius(v->tag, 8, 0);

    /* Note: a 25px card with a yellow bar, 2px below the row above. */
    v->note = box(ui.notes_box);
    lv_obj_set_size(v->note, LV_PCT(100), 27);
    lv_obj_set_style_pad_top(v->note, 2, 0);
    lv_obj_t *card = box(v->note);
    lv_obj_set_size(card, LV_PCT(100), 25);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_right(card, 8, 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    flex(card, LV_FLEX_FLOW_ROW, 9);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *bar = box(card);
    lv_obj_set_size(bar, 4, LV_PCT(100));
    lv_obj_set_style_radius(bar, 2, 0);
    bg(bar, lv_color_hex(NOTE_BAR), LV_OPA_COVER);
    v->note_label = text(card, F12I, "");
    lv_obj_set_width(v->note_label, 1);
    lv_obj_set_flex_grow(v->note_label, 1);
    lv_label_set_long_mode(v->note_label, LV_LABEL_LONG_DOT);
}

static void build_page_header(void)
{
    /* Shared header: tool icon, title, meta and close, 3px above the bubble padding. */
    ui.page_box = box(ui.bubble);
    lv_obj_set_width(ui.page_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.page_box, 1);
    lv_obj_set_style_pad_top(ui.page_box, -3, 0);
    lv_obj_add_flag(ui.page_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    flex(ui.page_box, LV_FLEX_FLOW_COLUMN, 5);
    lv_obj_t *head = box(ui.page_box);
    lv_obj_set_size(head, LV_PCT(100), 26);
    flex(head, LV_FLEX_FLOW_ROW, 7);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.page_icon = lv_img_create(head);
    lv_obj_set_size(ui.page_icon, 22, 22);
    ui.page_title = text(head, F11B, "");
    lv_obj_set_style_text_letter_space(ui.page_title, 1, 0);
    spacer(head);
    ui.page_meta = text(head, F11M, "");
    lv_label_set_long_mode(ui.page_meta, LV_LABEL_LONG_DOT);
    ui.page_close = tap_target(head, on_page_close, NULL);
    lv_obj_set_size(ui.page_close, 30, 26);
    lv_obj_set_style_radius(ui.page_close, 13, 0);
    lv_obj_set_ext_click_area(ui.page_close, 8);
    lv_obj_center(icon(ui.page_close, &arvel_icon_close));
}

static void build_timer_page(void)
{
    /* Timer: a 90px ring with the time left, then the label, rounds and controls. */
    ui.timer_box = page_body(LV_FLEX_FLOW_ROW, 14);
    lv_obj_t *ring = box(ui.timer_box);
    lv_obj_set_size(ring, 90, 90);
    ui.timer_ring = lv_arc_create(ring);
    lv_obj_remove_style_all(ui.timer_ring);
    lv_obj_clear_flag(ui.timer_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui.timer_ring, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(ui.timer_ring, 85, 85);
    lv_obj_center(ui.timer_ring);
    lv_arc_set_rotation(ui.timer_ring, 270);
    lv_arc_set_bg_angles(ui.timer_ring, 0, 360);
    lv_arc_set_range(ui.timer_ring, 0, 1000);
    lv_obj_set_style_arc_width(ui.timer_ring, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.timer_ring, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.timer_ring, true, LV_PART_INDICATOR);
    lv_obj_t *centre = box(ring);
    lv_obj_set_size(centre, 90, 90);
    flex(centre, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_align(centre, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.timer_time = text(centre, F22B, "");
    ui.timer_sub = text(centre, F10R, "");
    lv_obj_t *side = box(ui.timer_box);
    lv_obj_set_size(side, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(side, 1);
    flex(side, LV_FLEX_FLOW_COLUMN, 7);
    ui.timer_label = text(side, F14T, "");
    lv_obj_set_width(ui.timer_label, LV_PCT(100));
    lv_label_set_long_mode(ui.timer_label, LV_LABEL_LONG_DOT);
    lv_obj_t *dots = row(side, 5);
    for(int i = 0; i < TIMER_DOTS; ++i) {
        ui.timer_dots[i] = box(dots);
        lv_obj_set_size(ui.timer_dots[i], 10, 10);
        lv_obj_set_style_radius(ui.timer_dots[i], 5, 0);
    }
    ui.timer_rounds = text(dots, F11R, "");
    lv_obj_t *buttons = row(side, 6);
    for(int i = 0; i < 3; ++i) ui.timer_buttons[i] = page_button(buttons, on_timer_button, i, &ui.timer_button_labels[i]);
}

static void build_done_page(void)
{
    /* Timer done: a ringing bell in a soft halo, the news and what to do next. */
    ui.done_box = page_body(LV_FLEX_FLOW_ROW, 14);
    lv_obj_add_flag(ui.done_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    /* The design's 6px box-shadow spread: a faint 76px disc behind the 64px one. */
    lv_obj_t *halo = box(ui.done_box);
    lv_obj_set_size(halo, 64, 64);
    lv_obj_add_flag(halo, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *spread = box(halo);
    place(spread, -6, -6, 76, 76);
    lv_obj_set_style_radius(spread, LV_RADIUS_CIRCLE, 0);
    bg(spread, lv_color_hex(TIMER_RED), 36);
    ui.done_halo = box(halo);
    lv_obj_set_size(ui.done_halo, 64, 64);
    lv_obj_set_style_radius(ui.done_halo, LV_RADIUS_CIRCLE, 0);
    bg(ui.done_halo, lv_color_hex(DONE_HALO), LV_OPA_COVER);
    ui.bell_img = coloured(&arvel_icon_bell, lv_color_hex(BELL_RED));
    if(ui.bell_img) {
        lv_obj_t *bell = lv_img_create(ui.done_halo);
        lv_img_set_src(bell, ui.bell_img);
        lv_obj_center(bell);
        lv_img_set_pivot(bell, 15, 4);   /* the design swings it from (12, 3) of 24 */
        /* Built on demand, after the scene's tracks: kept out of the shared list a scene reload truncates. */
        ui.bell_track = (track_t){.obj = bell, .values = BELL_SWING, .key_times = BELL_TIMES, .period = 1800,
                                  .last = INT16_MIN, .count = 7, .prop = P_ANGLE, .group = G_BELL, .divisor = 1};
    }
    lv_obj_t *news = box(ui.done_box);
    lv_obj_set_size(news, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(news, 1);
    flex(news, LV_FLEX_FLOW_COLUMN, 4);
    ui.done_title = text(news, F19T, "");
    lv_obj_set_width(ui.done_title, LV_PCT(100));
    lv_label_set_long_mode(ui.done_title, LV_LABEL_LONG_DOT);
    ui.done_sub = text(news, F13R, "");
    lv_obj_set_width(ui.done_sub, LV_PCT(100));
    line_height(ui.done_sub, F13R, 18);
    lv_obj_t *done_buttons = row(news, 6);
    lv_obj_set_style_pad_top(done_buttons, 3, 0);
    for(int i = 0; i < 2; ++i) ui.done_buttons[i] = page_button(done_buttons, on_done_button, i, &ui.done_button_labels[i]);
}

static void build_notes_page(void)
{
    /* Notes and to-dos: a scrolling list of 23px rows. */
    ui.notes_box = lv_obj_create(ui.page_box);
    lv_obj_remove_style_all(ui.notes_box);
    lv_obj_set_width(ui.notes_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.notes_box, 1);
    lv_obj_set_scroll_dir(ui.notes_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.notes_box, LV_SCROLLBAR_MODE_OFF);
    flex(ui.notes_box, LV_FLEX_FLOW_COLUMN, 1);
    ui.notes_empty = text(ui.notes_box, F14R, "");
    lv_obj_set_width(ui.notes_empty, LV_PCT(100));
    line_height(ui.notes_empty, F14R, 19);
}

static void build_calc_page(void)
{
    /* Calculator: the sum, the answer and the units to show it in. */
    ui.calc_box = page_body(LV_FLEX_FLOW_COLUMN, 1);
    ui.calc_expr = text(ui.calc_box, M12, "");
    lv_obj_set_width(ui.calc_expr, LV_PCT(100));
    lv_label_set_long_mode(ui.calc_expr, LV_LABEL_LONG_DOT);
    ui.calc_value_row = box(ui.calc_box);
    lv_obj_set_size(ui.calc_value_row, LV_PCT(100), 39);
    flex(ui.calc_value_row, LV_FLEX_FLOW_ROW, 7);
    lv_obj_set_flex_align(ui.calc_value_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    ui.calc_value = text(ui.calc_value_row, F34B, "");
    lv_obj_set_style_text_letter_space(ui.calc_value, -1, 0);
    ui.calc_unit = text(ui.calc_value_row, F18S, "");
    /* Bottoms align in the row; this lifts the unit onto the answer's baseline. */
    lv_obj_set_style_pad_bottom(ui.calc_unit, F34B->base_line - F18S->base_line, 0);
    ui.calc_empty = text(ui.calc_box, F14R, "");
    lv_obj_set_width(ui.calc_empty, LV_PCT(100));
    line_height(ui.calc_empty, F14R, 19);
    lv_obj_set_style_pad_top(ui.calc_empty, 4, 0);
    set_text(ui.calc_empty, "Ask me to work something out — sums and unit conversions run right here, offline.");
    spacer(ui.calc_box);
    ui.calc_bottom = row(ui.calc_box, 10);
    ui.calc_seg = row(ui.calc_bottom, 2);
    lv_obj_set_style_pad_all(ui.calc_seg, 2, 0);
    lv_obj_set_style_radius(ui.calc_seg, 10, 0);
    for(int i = 0; i < 3; ++i) {
        ui.calc_opts[i] = tap_target(ui.calc_seg, on_calc_unit, (void *)(intptr_t)i);
        lv_obj_set_size(ui.calc_opts[i], LV_SIZE_CONTENT, 26);
        lv_obj_set_style_min_width(ui.calc_opts[i], 40, 0);
        lv_obj_set_style_pad_hor(ui.calc_opts[i], 10, 0);
        lv_obj_set_style_radius(ui.calc_opts[i], 8, 0);
        shadow(ui.calc_opts[i], 3, 1, 0, 0x000000);
        ui.calc_opt_labels[i] = text(ui.calc_opts[i], F12M, "");
        lv_obj_center(ui.calc_opt_labels[i]);
    }
    ui.calc_rate = text(ui.calc_bottom, M11, "");
}

static void build_weather_page(void)
{
    /* Weather: now on the left, the next four hours on the right, the day below. */
    ui.weather_box = page_body(LV_FLEX_FLOW_COLUMN, 4);
    ui.weather_main = box(ui.weather_box);
    lv_obj_set_width(ui.weather_main, LV_PCT(100));
    lv_obj_set_flex_grow(ui.weather_main, 1);
    flex(ui.weather_main, LV_FLEX_FLOW_ROW, 12);
    lv_obj_set_flex_align(ui.weather_main, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *now = row(ui.weather_main, 6);
    lv_obj_set_width(now, 148);
    ui.weather_icon = lv_img_create(now);
    lv_obj_set_size(ui.weather_icon, 54, 54);
    lv_obj_t *reading = box(now);
    lv_obj_set_size(reading, 88, LV_SIZE_CONTENT);
    flex(reading, LV_FLEX_FLOW_COLUMN, 0);
    ui.weather_temp = text(reading, F34B, "");
    lv_obj_set_style_text_letter_space(ui.weather_temp, -1, 0);
    line_height(ui.weather_temp, F34B, 36);
    ui.weather_condition = text(reading, F11R, "");
    lv_obj_set_width(ui.weather_condition, LV_PCT(100));
    lv_label_set_long_mode(ui.weather_condition, LV_LABEL_LONG_DOT);
    lv_obj_t *rule = box(ui.weather_main);
    lv_obj_set_size(rule, 1, 62);
    lv_obj_t *hours = box(ui.weather_main);
    lv_obj_set_size(hours, 1, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(hours, 1);
    flex(hours, LV_FLEX_FLOW_ROW, 4);
    for(int i = 0; i < DESK_HOURS; ++i) {
        lv_obj_t *hour = box(hours);
        lv_obj_set_size(hour, 1, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(hour, 1);
        flex(hour, LV_FLEX_FLOW_COLUMN, 2);
        lv_obj_set_flex_align(hour, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ui.weather_hour_time[i] = text(hour, F11R, "");
        ui.weather_hour_icon[i] = lv_img_create(hour);
        lv_obj_set_size(ui.weather_hour_icon[i], 26, 26);
        ui.weather_hour_temp[i] = text(hour, F13S, "");
    }
    ui.weather_footer = text(ui.weather_box, F11R, "");
    lv_obj_set_width(ui.weather_footer, LV_PCT(100));
    lv_label_set_long_mode(ui.weather_footer, LV_LABEL_LONG_DOT);
    ui.weather_empty = text(ui.weather_box, F14R, "Ask me for the weather somewhere — “what’s the weather in Copenhagen?” — and it shows up here.");
    lv_obj_set_width(ui.weather_empty, LV_PCT(100));
    line_height(ui.weather_empty, F14R, 19);
    lv_obj_set_style_pad_top(ui.weather_empty, 4, 0);
}

/* Pages are built the first time they open, then kept. */
static void ensure_page(ui_mode_t mode)
{
    if(!page_mode(mode)) return;
    if(!ui.page_box) build_page_header();
    switch(mode) {
        case MODE_TIMER: if(ui.timer_box) return; build_timer_page(); break;
        case MODE_TIMER_DONE: if(ui.done_box) return; build_done_page(); break;
        case MODE_NOTES: if(ui.notes_box) return; build_notes_page(); break;
        case MODE_CALC: if(ui.calc_box) return; build_calc_page(); break;
        default: if(ui.weather_box) return; build_weather_page(); break;
    }
    apply_page_colors();
}

static void build_bubble(void)
{
    ui.bubble = box(ui.screen);
    place(ui.bubble, 176, 12, 396, 148);
    lv_obj_set_style_radius(ui.bubble, 16, 0);
    lv_obj_set_style_pad_top(ui.bubble, 11, 0);
    lv_obj_set_style_pad_bottom(ui.bubble, 12, 0);
    lv_obj_set_style_pad_hor(ui.bubble, 16, 0);
    shadow(ui.bubble, 18, 6, 36, SHADOW_INK);
    flex(ui.bubble, LV_FLEX_FLOW_COLUMN, 5);

    /* The tail is drawn above the bubble, like the design's child square. */
    ui.tail = icon(ui.screen, &arvel_icon_tail);
    lv_obj_set_pos(ui.tail, 176 + 1 - 12, 12 + 65 - 12);

    ui.status_row = box(ui.bubble);
    lv_obj_set_size(ui.status_row, LV_PCT(100), 16);
    flex(ui.status_row, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(ui.status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.status_dot = box(ui.status_row);
    lv_obj_set_size(ui.status_dot, 7, 7);
    lv_obj_set_style_radius(ui.status_dot, 4, 0);
    lv_obj_set_style_bg_opa(ui.status_dot, LV_OPA_COVER, 0);
    ui.status_label = text(ui.status_row, F11B, "READY");
    lv_obj_set_style_text_letter_space(ui.status_label, 1, 0);
    spacer(ui.status_row);
    ui.model_label = text(ui.status_row, F11M, "");
    lv_label_set_long_mode(ui.model_label, LV_LABEL_LONG_DOT);

    /* Idle: greeting, nudge and the last request. */
    ui.idle_box = box(ui.bubble);
    lv_obj_set_width(ui.idle_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.idle_box, 1);
    flex(ui.idle_box, LV_FLEX_FLOW_COLUMN, 5);
    ui.idle_title = text(ui.idle_box, F21B, "");
    lv_obj_set_width(ui.idle_title, LV_PCT(100));
    lv_label_set_long_mode(ui.idle_title, LV_LABEL_LONG_DOT);
    line_height(ui.idle_title, F21B, 25);
    ui.idle_sub = text(ui.idle_box, F14R, "");
    lv_obj_set_width(ui.idle_sub, LV_PCT(100));
    line_height(ui.idle_sub, F14R, 19);
    spacer(ui.idle_box);
    /* Shortcuts to the tool pages; like the design, the row may overhang the content box by a pixel or two. */
    lv_obj_add_flag(ui.idle_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    ui.suggest_row = row(ui.idle_box, 5);
    for(int i = 0; i < SUGGESTIONS; ++i) {
        lv_obj_t *chip = ui.suggest[i] = tap_target(ui.suggest_row, on_suggest, (void *)(intptr_t)i);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, 28);
        lv_obj_set_style_radius(chip, 14, 0);
        lv_obj_set_style_pad_left(chip, 3, 0);
        lv_obj_set_style_pad_right(chip, 10, 0);
        flex(chip, LV_FLEX_FLOW_ROW, 5);
        lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *glyph = lv_img_create(chip);
        lv_obj_set_size(glyph, 22, 22);
        lv_obj_add_flag(glyph, LV_OBJ_FLAG_EVENT_BUBBLE);
        set_tool_icon(glyph, SUGGEST[i].kind, 0);
        text(chip, F12S, SUGGEST[i].label);
    }

    /* Listening: what Arvel is hearing, a caret and the level meter. */
    ui.listen_box = box(ui.bubble);
    lv_obj_set_width(ui.listen_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.listen_box, 1);
    flex(ui.listen_box, LV_FLEX_FLOW_COLUMN, 6);
    ui.listen_words = text(ui.listen_box, F18D, "Go ahead — I’m listening #A65B48 |#");
    lv_label_set_recolor(ui.listen_words, true);
    lv_obj_set_width(ui.listen_words, LV_PCT(100));
    lv_obj_set_style_min_height(ui.listen_words, 48, 0);
    line_height(ui.listen_words, F18D, 23);
    spacer(ui.listen_box);
    lv_obj_t *meter_row = box(ui.listen_box);
    lv_obj_set_size(meter_row, LV_PCT(100), 24);
    flex(meter_row, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_flex_align(meter_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *meter = box(meter_row);
    lv_obj_set_size(meter, 132, 24);
    for(int i = 0; i < 10; ++i) {
        lv_obj_t *bar = box(meter);
        place(bar, BARS[i].x, BARS[i].y[0], 4, BARS[i].h[0]);
        lv_obj_set_style_radius(bar, 2, 0);
        bg(bar, lv_color_hex(MIC_RED), LV_OPA_COVER);
        add_track(bar, G_LISTEN, P_H, BARS[i].h, BARS[i].count, BARS[i].period, 0);
        add_track(bar, G_LISTEN, P_Y, BARS[i].y, BARS[i].count, BARS[i].period, 0);
    }
    text(meter_row, F12R, "Tap ■ when you’re done");

    /* Persistent voice uses actual provider captions, never a simulated meter. */
    ui.live_box = box(ui.bubble);
    lv_obj_set_width(ui.live_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.live_box, 1);
    flex(ui.live_box, LV_FLEX_FLOW_COLUMN, 6);
    ui.live_role = text(ui.live_box, F11B, "");
    ui.live_scroll = lv_obj_create(ui.live_box);
    lv_obj_remove_style_all(ui.live_scroll);
    lv_obj_set_width(ui.live_scroll, LV_PCT(100));
    lv_obj_set_flex_grow(ui.live_scroll, 1);
    lv_obj_set_scroll_dir(ui.live_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.live_scroll, LV_SCROLLBAR_MODE_OFF);
    ui.live_words = text(ui.live_scroll, F17D, "");
    lv_obj_set_width(ui.live_words, LV_PCT(100));
    line_height(ui.live_words, F17D, 23);
    ui.live_empty = text(ui.live_box, F18D, "Voice chat is open");
    lv_obj_set_width(ui.live_empty, LV_PCT(100));
    line_height(ui.live_empty, F18D, 23);
    spacer(ui.live_box);
    ui.live_hint = text(ui.live_box, F12R, "Mic on · hold mic to end");

    /* Thinking: the step, the request and where it runs. */
    ui.think_box = box(ui.bubble);
    lv_obj_set_width(ui.think_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.think_box, 1);
    flex(ui.think_box, LV_FLEX_FLOW_COLUMN, 6);
    lv_obj_t *think_row = box(ui.think_box);
    lv_obj_set_size(think_row, LV_PCT(100), LV_SIZE_CONTENT);
    flex(think_row, LV_FLEX_FLOW_ROW, 10);
    lv_obj_set_flex_align(think_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.think_title = text(think_row, F17B, "");
    lv_obj_t *dots = box(think_row);
    lv_obj_set_size(dots, 30, 10);
    for(int i = 0; i < 3; ++i) {
        lv_obj_t *dot = ui.think_dots[i] = box(dots);
        place(dot, 2 + 10 * i, 2, 7, 7);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        add_track(dot, G_THINK, P_OPA, DOT_OPA, 3, 1200, (uint16_t)(200 * i));
    }
    ui.think_tools = box(ui.think_box);
    lv_obj_set_size(ui.think_tools, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(ui.think_tools, 1);
    flex(ui.think_tools, LV_FLEX_FLOW_COLUMN, 4);
    for(int i = 0; i < THINK_ROWS; ++i) build_tool_view(&ui.think_rows[i], ui.think_tools, false);
    ui.think_quote = text(ui.think_box, F14R, "");
    lv_obj_set_width(ui.think_quote, LV_PCT(100));
    lv_obj_set_height(ui.think_quote, 38);
    lv_label_set_long_mode(ui.think_quote, LV_LABEL_LONG_DOT);
    line_height(ui.think_quote, F14R, 19);
    ui.think_spacer = spacer(ui.think_box);
    ui.think_footer = box(ui.think_box);
    lv_obj_set_size(ui.think_footer, LV_PCT(100), LV_SIZE_CONTENT);
    flex(ui.think_footer, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(ui.think_footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icon(ui.think_footer, &arvel_icon_shield);
    lv_obj_t *where = text(ui.think_footer, F12R, "");
    lv_obj_set_flex_grow(where, 1);
    lv_label_set_long_mode(where, LV_LABEL_LONG_DOT);

    /* Speaking: captions highlighted word by word over the full reply. */
    ui.speak_box = box(ui.bubble);
    lv_obj_set_width(ui.speak_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.speak_box, 1);
    flex(ui.speak_box, LV_FLEX_FLOW_COLUMN, 6);
    ui.speak_scroll = lv_obj_create(ui.speak_box);
    lv_obj_remove_style_all(ui.speak_scroll);
    lv_obj_set_width(ui.speak_scroll, LV_PCT(100));
    lv_obj_set_flex_grow(ui.speak_scroll, 1);
    lv_obj_set_scroll_dir(ui.speak_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.speak_scroll, LV_SCROLLBAR_MODE_OFF);
    ui.speak_faint = text(ui.speak_scroll, F17D, "");
    ui.speak_ink = text(ui.speak_scroll, F17D, "");
    lv_obj_t *speak_labels[] = {ui.speak_faint, ui.speak_ink};
    for(int i = 0; i < 2; ++i) {
        lv_obj_set_pos(speak_labels[i], 0, 0);
        lv_obj_set_width(speak_labels[i], 364);
        line_height(speak_labels[i], F17D, 23);
        lv_obj_clear_flag(speak_labels[i], LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    lv_obj_t *speak_footer = box(ui.speak_box);
    lv_obj_set_size(speak_footer, LV_PCT(100), 16);
    flex(speak_footer, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(speak_footer, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.speak_icons = box(speak_footer);
    lv_obj_set_size(ui.speak_icons, 15, 15);
    icon(ui.speak_icons, &arvel_icon_speaker);
    lv_obj_t *wave1 = icon(ui.speak_icons, &arvel_icon_wave1);
    lv_obj_t *wave2 = icon(ui.speak_icons, &arvel_icon_wave2);
    add_track(wave1, G_SPEAK, P_OPA, WAVE_OPA, 3, 900, 0);
    add_track(wave2, G_SPEAK, P_OPA, WAVE_OPA, 3, 900, 300);
    ui.speak_hint = text(speak_footer, F12R, "");

    /* Chat log: one question and answer per page. */
    ui.chat_box = box(ui.bubble);
    lv_obj_set_width(ui.chat_box, LV_PCT(100));
    lv_obj_set_flex_grow(ui.chat_box, 1);
    flex(ui.chat_box, LV_FLEX_FLOW_COLUMN, 5);
    lv_obj_t *chat_head = box(ui.chat_box);
    lv_obj_set_size(chat_head, LV_PCT(100), 28);
    lv_obj_set_style_pad_top(ui.chat_box, -4, 0);
    flex(chat_head, LV_FLEX_FLOW_ROW, 6);
    lv_obj_set_flex_align(chat_head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.chat_dot = box(chat_head);
    lv_obj_set_size(ui.chat_dot, 7, 7);
    lv_obj_set_style_radius(ui.chat_dot, 4, 0);
    ui.chat_time = text(chat_head, F11B, "TODAY");
    lv_obj_set_style_text_letter_space(ui.chat_time, 1, 0);
    spacer(chat_head);
    ui.chat_dots = dots_row(chat_head, ui.page_dots, MAX_MESSAGES);
    ui.chat_older = pill_button(chat_head, &arvel_icon_chevron_up, on_chat_page, -1);
    ui.chat_newer = pill_button(chat_head, &arvel_icon_chevron_down, on_chat_page, 1);
    ui.chat_content = lv_obj_create(ui.chat_box);
    lv_obj_remove_style_all(ui.chat_content);
    lv_obj_set_width(ui.chat_content, LV_PCT(100));
    lv_obj_set_flex_grow(ui.chat_content, 1);
    lv_obj_set_scroll_dir(ui.chat_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.chat_content, LV_SCROLLBAR_MODE_OFF);
    flex(ui.chat_content, LV_FLEX_FLOW_COLUMN, 5);
    ui.chat_q_row = box(ui.chat_content);
    lv_obj_set_size(ui.chat_q_row, LV_PCT(100), LV_SIZE_CONTENT);
    flex(ui.chat_q_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_align(ui.chat_q_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    ui.chat_q = box(ui.chat_q_row);
    lv_obj_set_style_radius(ui.chat_q, 12, 0);
    /* Square-ish bottom-right corner (radius 4) like the design's 12 12 4 12. */
    ui.chat_q_corner = box(ui.chat_q);
    lv_obj_set_size(ui.chat_q_corner, 12, 12);
    lv_obj_set_style_radius(ui.chat_q_corner, 4, 0);
    ui.chat_q_label = text(ui.chat_q, F13R, "");
    lv_obj_set_pos(ui.chat_q_label, 10, 5);
    line_height(ui.chat_q_label, F13R, 18);
    ui.chat_tools = box(ui.chat_content);
    lv_obj_set_size(ui.chat_tools, LV_PCT(100), 24);
    flex(ui.chat_tools, LV_FLEX_FLOW_ROW, 5);
    for(int i = 0; i < CHAT_CHIPS; ++i) build_tool_view(&ui.chat_chips[i], ui.chat_tools, true);
    /* The reply, after a 3px accent bar and a 7px gap. */
    ui.chat_a = text(ui.chat_content, F13R, "");
    lv_obj_set_width(ui.chat_a, LV_PCT(100));
    line_height(ui.chat_a, F13R, 18);
    lv_obj_set_style_border_side(ui.chat_a, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(ui.chat_a, 3, 0);
    lv_obj_set_style_border_opa(ui.chat_a, 153, 0);
    lv_obj_set_style_pad_left(ui.chat_a, 10, 0);
    lv_obj_clear_flag(ui.chat_a, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static lv_obj_t *panel(void)
{
    lv_obj_t *obj = box(ui.screen);
    place(obj, 12, 12, 560, 148);
    lv_obj_set_style_radius(obj, 16, 0);
    shadow(obj, 18, 6, 36, SHADOW_INK);
    return obj;
}

static void build_scenes_panel(void)
{
    ui.scenes_panel = panel();
    lv_obj_set_style_pad_ver(ui.scenes_panel, 12, 0);
    lv_obj_set_style_pad_hor(ui.scenes_panel, 14, 0);
    flex(ui.scenes_panel, LV_FLEX_FLOW_COLUMN, 10);
    lv_obj_t *head = box(ui.scenes_panel);
    lv_obj_set_size(head, LV_PCT(100), 28);
    flex(head, LV_FLEX_FLOW_ROW, 8);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui.scenes_title = text(head, F15B, "");
    spacer(head);
    dots_row(head, ui.scene_dots, 2);
    ui.scene_prev = pill_button(head, &arvel_icon_chevron_left, on_scene_page, -1);
    ui.scene_next = pill_button(head, &arvel_icon_chevron_right, on_scene_page, 1);
    lv_obj_t *grid = box(ui.scenes_panel);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    flex(grid, LV_FLEX_FLOW_ROW, 8);
    for(int i = 0; i < SCENES_PER_PAGE; ++i) {
        lv_obj_t *tile = tap_target(grid, on_scene_pick, (void *)(intptr_t)i);
        lv_obj_set_size(tile, 82, LV_SIZE_CONTENT);
        flex(tile, LV_FLEX_FLOW_COLUMN, 6);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ui.scene_tiles[i] = tile;
        ui.scene_rings[i] = box(tile);
        lv_obj_set_size(ui.scene_rings[i], 80, 56);
        lv_obj_set_style_radius(ui.scene_rings[i], 10, 0);
        ui.scene_thumbs[i] = lv_img_create(ui.scene_rings[i]);
        lv_obj_add_flag(ui.scene_thumbs[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        ui.scene_names[i] = text(tile, F12M, "");
    }
}

static void build_settings_panel(void)
{
    ui.settings_panel = panel();
    lv_obj_set_style_pad_top(ui.settings_panel, 10, 0);
    lv_obj_set_style_pad_bottom(ui.settings_panel, 10, 0);
    lv_obj_set_style_pad_left(ui.settings_panel, 10, 0);
    lv_obj_set_style_pad_right(ui.settings_panel, 14, 0);
    flex(ui.settings_panel, LV_FLEX_FLOW_ROW, 12);
    lv_obj_t *nav = box(ui.settings_panel);
    ui.settings_nav = nav;
    lv_obj_set_size(nav, 112, LV_PCT(100));
    flex(nav, LV_FLEX_FLOW_COLUMN, 3);
    lv_obj_t *title = text(nav, F15B, "Settings");
    lv_obj_set_height(title, 26);
    lv_obj_set_style_pad_top(title, 3, 0);
    lv_obj_set_style_pad_hor(title, 8, 0);
    for(int t = 0; t < TAB_COUNT; ++t) {
        ui.tabs[t] = tap_target(nav, on_tab, (void *)(intptr_t)t);
        /* 26px in the design, shrunk by its flex column to fit the panel. */
        lv_obj_set_size(ui.tabs[t], LV_PCT(100), 22);
        lv_obj_set_style_radius(ui.tabs[t], 8, 0);
        lv_obj_set_style_pad_hor(ui.tabs[t], 9, 0);
        ui.tab_labels[t] = text(ui.tabs[t], F13M, "");
        lv_obj_set_width(ui.tab_labels[t], LV_PCT(100));
        lv_label_set_long_mode(ui.tab_labels[t], LV_LABEL_LONG_DOT);
        lv_obj_align(ui.tab_labels[t], LV_ALIGN_LEFT_MID, 0, 0);
    }
    ui.settings_rule = box(ui.settings_panel);
    lv_obj_set_size(ui.settings_rule, 1, LV_PCT(100));
    lv_obj_t *list = box(ui.settings_panel);
    ui.settings_list = list;
    lv_obj_set_height(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    flex(list, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for(int i = 0; i < ROWS_PER_TAB; ++i) {
        row_slot_t *r = &ui.rows[i];
        r->row = box(list);
        lv_obj_set_size(r->row, LV_PCT(100), 32);
        lv_obj_set_style_border_side(r->row, LV_BORDER_SIDE_BOTTOM, 0);
        flex(r->row, LV_FLEX_FLOW_ROW, 8);
        lv_obj_set_flex_align(r->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        r->label = text(r->row, F13D, "");
        lv_obj_set_flex_grow(r->label, 1);
        lv_label_set_long_mode(r->label, LV_LABEL_LONG_DOT);

        r->toggle = tap_target(r->row, on_toggle, (void *)(intptr_t)i);
        lv_obj_set_size(r->toggle, 40, 24);
        lv_obj_set_style_radius(r->toggle, 12, 0);
        lv_obj_set_ext_click_area(r->toggle, 6);
        r->knob = box(r->toggle);
        place(r->knob, 3, 3, 18, 18);
        lv_obj_set_style_radius(r->knob, 9, 0);
        bg(r->knob, lv_color_white(), LV_OPA_COVER);
        shadow(r->knob, 3, 1, 64, 0x000000);

        r->stepper = box(r->row);
        lv_obj_set_size(r->stepper, LV_SIZE_CONTENT, 26);
        flex(r->stepper, LV_FLEX_FLOW_ROW, 4);
        lv_obj_set_flex_align(r->stepper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for(int j = 0; j < 2; ++j) {
            lv_obj_t *step = tap_target(r->stepper, on_step, (void *)(intptr_t)(i + 8 * j));
            lv_obj_set_size(step, 28, 26);
            lv_obj_set_style_radius(step, 13, 0);
            lv_obj_set_ext_click_area(step, 4);
            lv_obj_center(text(step, F15S, j ? "+" : "−"));
            if(j == 0) {
                r->step_value = text(r->stepper, F13S, "");
                lv_obj_set_width(r->step_value, 44);
                lv_obj_set_style_text_align(r->step_value, LV_TEXT_ALIGN_CENTER, 0);
            }
        }

        r->seg = box(r->row);
        lv_obj_set_size(r->seg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(r->seg, 2, 0);
        lv_obj_set_style_radius(r->seg, 9, 0);
        flex(r->seg, LV_FLEX_FLOW_ROW, 2);
        for(int j = 0; j < 3; ++j) {
            r->seg_opts[j] = tap_target(r->seg, on_seg, (void *)(intptr_t)(i * 4 + j));
            lv_obj_set_size(r->seg_opts[j], LV_SIZE_CONTENT, 24);
            lv_obj_set_style_pad_hor(r->seg_opts[j], 9, 0);
            lv_obj_set_style_radius(r->seg_opts[j], 7, 0);
            shadow(r->seg_opts[j], 3, 1, 0, 0x000000);
            r->seg_labels[j] = text(r->seg_opts[j], F12M, "");
            lv_obj_align(r->seg_labels[j], LV_ALIGN_LEFT_MID, 0, 0);
        }

        r->value = tap_target(r->row, on_value, (void *)(intptr_t)i);
        lv_obj_set_size(r->value, LV_SIZE_CONTENT, 28);
        lv_obj_set_style_pad_hor(r->value, 2, 0);
        flex(r->value, LV_FLEX_FLOW_ROW, 4);
        lv_obj_set_flex_align(r->value, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        r->value_label = text(r->value, F13R, "");
        lv_label_set_long_mode(r->value_label, LV_LABEL_LONG_DOT);
        icon(r->value, &arvel_icon_chevron_right_small);
    }
    ui.wifi_setup = box(ui.settings_panel);
    lv_obj_set_size(ui.wifi_setup, LV_PCT(100), LV_PCT(100));
    /* Settings has a 536x128 content area: header, two steps, then 44px targets. */
    ui.wifi_setup_title = text(ui.wifi_setup, F15B, "Wi-Fi setup");
    place(ui.wifi_setup_title, 0, 0, 260, 24);
    ui.wifi_setup_status = text(ui.wifi_setup, F13S, "");
    place(ui.wifi_setup_status, 336, 0, 200, 24);
    lv_obj_set_style_radius(ui.wifi_setup_status, 6, 0);
    lv_obj_set_style_pad_hor(ui.wifi_setup_status, 8, 0);
    lv_obj_set_style_pad_ver(ui.wifi_setup_status, 3, 0);
    lv_obj_set_style_text_align(ui.wifi_setup_status, LV_TEXT_ALIGN_RIGHT, 0);
    ui.wifi_setup_join = text(ui.wifi_setup, F13S, "1  Join Unravel-Agent");
    place(ui.wifi_setup_join, 0, 32, 260, 18);
    ui.wifi_setup_key = text(ui.wifi_setup, F21B, "");
    place(ui.wifi_setup_key, 0, 52, 260, 28);
    ui.wifi_setup_caption = text(ui.wifi_setup, F11M, "Password");
    place(ui.wifi_setup_caption, 144, 60, 100, 16);
    ui.wifi_setup_url = text(ui.wifi_setup, F13S, "");
    place(ui.wifi_setup_url, 276, 32, 260, 18);
    ui.wifi_setup_hint = text(ui.wifi_setup, F12M, "");
    place(ui.wifi_setup_hint, 276, 52, 260, 32);
    line_height(ui.wifi_setup_hint, F12M, 16);
    static const char *const labels[] = {"New password", "Stop setup", "Back"};
    static const int widths[] = {184, 176, 152};
    int x = 0;
    for(int i = 0; i < 3; ++i) {
        ui.wifi_setup_buttons[i] = tap_target(ui.wifi_setup, on_wifi_setup, (void *)(intptr_t)i);
        place(ui.wifi_setup_buttons[i], x, 84, widths[i], 44);
        lv_obj_set_style_radius(ui.wifi_setup_buttons[i], 8, 0);
        lv_obj_set_style_border_width(ui.wifi_setup_buttons[i], i == 2 ? 1 : 0, 0);
        lv_obj_set_style_opa(ui.wifi_setup_buttons[i], LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_style_outline_width(ui.wifi_setup_buttons[i], 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(ui.wifi_setup_buttons[i], 2, LV_STATE_FOCUS_KEY);
        ui.wifi_setup_labels[i] = text(ui.wifi_setup_buttons[i], F13S, labels[i]);
        lv_obj_center(ui.wifi_setup_labels[i]);
        x += widths[i] + 12;
    }
    ui.web_login = box(ui.settings_panel);
    lv_obj_set_size(ui.web_login, LV_PCT(100), LV_PCT(100));
    ui.web_login_title = text(ui.web_login, F15B, "Web login");
    place(ui.web_login_title, 0, 0, 260, 24);
    ui.web_login_status = text(ui.web_login, F13S, "");
    place(ui.web_login_status, 304, 0, 232, 24);
    lv_obj_set_style_radius(ui.web_login_status, 6, 0);
    lv_obj_set_style_pad_hor(ui.web_login_status, 8, 0);
    lv_obj_set_style_pad_ver(ui.web_login_status, 3, 0);
    lv_obj_set_style_text_align(ui.web_login_status, LV_TEXT_ALIGN_RIGHT, 0);
    ui.web_login_code = text(ui.web_login, F21B, "");
    place(ui.web_login_code, 0, 24, 128, 28);
    ui.web_login_caption = text(ui.web_login, F11M, "Approval code\nNot Wi-Fi password");
    place(ui.web_login_caption, 136, 24, 132, 28);
    line_height(ui.web_login_caption, F11M, 14);
    ui.web_login_url = text(ui.web_login, F12S, "");
    place(ui.web_login_url, 276, 24, 260, 28);
    line_height(ui.web_login_url, F12S, 14);
    ui.web_login_hint = text(ui.web_login, F12M, "");
    place(ui.web_login_hint, 0, 52, 536, 32);
    line_height(ui.web_login_hint, F12M, 16);
    static const char *const web_labels[] = {"New code", "Cancel", "Back"};
    x = 0;
    for(int i = 0; i < 3; ++i) {
        ui.web_login_buttons[i] = tap_target(ui.web_login, on_web_login, (void *)(intptr_t)i);
        place(ui.web_login_buttons[i], x, 84, widths[i], 44);
        lv_obj_set_style_radius(ui.web_login_buttons[i], 8, 0);
        lv_obj_set_style_border_width(ui.web_login_buttons[i], i == 2 ? 1 : 0, 0);
        lv_obj_set_style_opa(ui.web_login_buttons[i], LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_style_outline_width(ui.web_login_buttons[i], 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_pad(ui.web_login_buttons[i], 2, LV_STATE_FOCUS_KEY);
        ui.web_login_labels[i] = text(ui.web_login_buttons[i], F13S, web_labels[i]);
        lv_obj_center(ui.web_login_labels[i]);
        x += widths[i] + 12;
    }
    ui.maintenance = box(ui.settings_panel);
    lv_obj_set_size(ui.maintenance, LV_PCT(100), LV_PCT(100));
    flex(ui.maintenance, LV_FLEX_FLOW_COLUMN, 8);
    ui.maintenance_title = text(ui.maintenance, F13S, "");
    ui.maintenance_detail = text(ui.maintenance, F12M, "");
    lv_obj_set_width(ui.maintenance_detail, LV_PCT(100));
    ui.maintenance_status = text(ui.maintenance, F11M, "");
    lv_obj_set_width(ui.maintenance_status, LV_PCT(100));
    lv_obj_t *maintenance_actions = box(ui.maintenance);
    lv_obj_set_size(maintenance_actions, LV_SIZE_CONTENT, 32);
    flex(maintenance_actions, LV_FLEX_FLOW_ROW, 8);
    for(int i = 0; i < 3; ++i) {
        ui.maintenance_buttons[i] = tap_target(maintenance_actions, on_maintenance, (void *)(intptr_t)i);
        lv_obj_set_size(ui.maintenance_buttons[i], i == 2 ? 100 : 140, 32);
        lv_obj_set_style_radius(ui.maintenance_buttons[i], 8, 0);
        ui.maintenance_labels[i] = text(ui.maintenance_buttons[i], F12S, "");
        lv_obj_center(ui.maintenance_labels[i]);
    }
}

static void build_rail(void)
{
    ui.scene_btn = round_button(ui.screen, 12, on_scenes);
    ui.scene_icon = icon(ui.scene_btn, &arvel_icon_image);
    lv_obj_center(ui.scene_icon);
    ui.end_btn = round_button(ui.screen, 12, on_end);
    lv_obj_t *end_label = text(ui.end_btn, F12S, "End");
    lv_obj_center(end_label);
    bg(ui.end_btn, lv_color_hex(MIC_RED), LV_OPA_COVER);
    ink(ui.end_btn, lv_color_hex(0xF8F6F0));
    hidden(ui.end_btn, true);
    ui.chat_btn = round_button(ui.screen, 64, on_chat);
    ui.chat_icon = icon(ui.chat_btn, &arvel_icon_chat);
    lv_obj_center(ui.chat_icon);
    ui.mic_btn = round_button(ui.screen, 116, on_mic);
    lv_obj_add_event_cb(ui.mic_btn, on_mic, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui.mic_btn, on_mic, LV_EVENT_LONG_PRESSED, NULL);
    bg(ui.mic_btn, lv_color_hex(MIC_RED), LV_OPA_COVER);
    shadow(ui.mic_btn, 10, 3, 77, 0x783223);
    ui.mic_icon = icon(ui.mic_btn, &arvel_icon_mic);
    ui.stop_icon = icon(ui.mic_btn, &arvel_icon_stop);
    lv_obj_center(ui.mic_icon);
    lv_obj_center(ui.stop_icon);
    tint(ui.mic_icon, lv_color_white());
    tint(ui.stop_icon, lv_color_white());
    static const lv_point_t mute_points[] = {{12, 12}, {32, 32}};
    ui.mute_slash = lv_line_create(ui.mic_btn);
    lv_obj_remove_style_all(ui.mute_slash);
    lv_line_set_points(ui.mute_slash, mute_points, 2);
    lv_obj_set_style_line_width(ui.mute_slash, 2, 0);
    lv_obj_set_style_line_color(ui.mute_slash, lv_color_hex(0xF8F6F0), 0);
    lv_obj_clear_flag(ui.mute_slash, LV_OBJ_FLAG_CLICKABLE);
}

/* ---------------------------------------------------------------- public API */

void buddy_ui_init(const buddy_ui_callbacks_t *callbacks)
{
    if(ui.screen) {
        if(callbacks) ui.callbacks = *callbacks;
        else memset(&ui.callbacks, 0, sizeof(ui.callbacks));
        return;
    }
    memset(&ui, 0, sizeof(ui));
    if(callbacks) ui.callbacks = *callbacks;
    ui.prefs = (buddy_ui_prefs_t){0, 10, true, 70, true, 1, 0, 60, false};
    ui.wpm = 175;
    ui.hour = 9;
    ui.chat_page = -1;

    ui.screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(ui.screen);
    lv_obj_clear_flag(ui.screen, LV_OBJ_FLAG_SCROLLABLE);
    bg(ui.screen, lv_color_hex(0xDCEBE4), LV_OPA_COVER);
    lv_obj_set_style_text_font(ui.screen, F13R, 0);

    ui.scene_box = box(ui.screen);
    place(ui.scene_box, 0, 0, SCREEN_W, SCREEN_H);
    build_character();
    build_pill();
    build_bubble();
    build_scenes_panel();
    build_settings_panel();
    build_rail();

    ui.wake_catcher = tap_target(lv_layer_top(), on_wake, NULL);
    place(ui.wake_catcher, 0, 0, SCREEN_W, SCREEN_H);
    hidden(ui.wake_catcher, true);

    ui.scene_track_start = ui.track_count;
    group(G_SCENE, true);
    ui.save_timer = lv_timer_create(save_now, SAVE_DELAY_MS, NULL);
    lv_timer_pause(ui.save_timer);
    make_palette(false);
    load_scene(0);
    if(!ui.scene_loaded) apply_colors();
    set_mode(MODE_IDLE);
    ui.frame_timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
    lv_scr_load(ui.screen);
}

void buddy_ui_set_profile(const buddy_ui_profile_t *profile)
{
    if(!ui.screen || !profile) return;
    const char *storage = profile->storage_state ? profile->storage_state : "";
    const char *conversation = profile->conversation_id ? profile->conversation_id : "";
    const char *readiness = profile->native_readiness ? profile->native_readiness : "";
    if(strcmp(ui.buddy_name, profile->buddy_name ? profile->buddy_name : "") == 0 &&
       strcmp(ui.user_name, profile->user_name ? profile->user_name : "") == 0 &&
       strcmp(ui.model, profile->model ? profile->model : "") == 0 &&
       strcmp(ui.firmware, profile->firmware ? profile->firmware : "") == 0 &&
       ui.chat_configured == profile->chat_configured && ui.voice_configured == profile->voice_configured &&
       strcmp(ui.storage_state, storage) == 0 && strcmp(ui.conversation_id, conversation) == 0 &&
       strcmp(ui.native_readiness, readiness) == 0) return;
    copy(ui.buddy_name, sizeof(ui.buddy_name), profile->buddy_name);
    copy(ui.user_name, sizeof(ui.user_name), profile->user_name);
    copy(ui.model, sizeof(ui.model), profile->model);
    copy(ui.firmware, sizeof(ui.firmware), profile->firmware);
    copy(ui.storage_state, sizeof(ui.storage_state), storage);
    copy(ui.conversation_id, sizeof(ui.conversation_id), conversation);
    copy(ui.native_readiness, sizeof(ui.native_readiness), readiness);
    ui.chat_configured = profile->chat_configured;
    ui.voice_configured = profile->voice_configured;
    refresh_status();
    refresh_scenes();
    refresh_settings();
}

void buddy_ui_set_prefs(const buddy_ui_prefs_t *prefs, const char *scene)
{
    if(!ui.screen || !prefs || ui.dirty) return;
    ui.prefs = *prefs;
    ui.prefs_known = true;
    for(size_t i = 0; i < ARVEL_SCENE_COUNT; ++i) {
        if(scene && strcmp(scene, arvel_scenes[i].id) == 0) { load_scene(i); break; }
    }
    refresh_settings();
}

void buddy_ui_set_status(buddy_ui_state_t state, const char *detail, bool busy)
{
    if(!ui.screen) return;
    if(state == ui.state && busy == ui.busy &&
       strcmp(ui.detail, detail ? detail : "") == 0) return;
    const buddy_ui_state_t previous = ui.state;
    ui.state = state;
    ui.busy = busy;
    copy(ui.detail, sizeof(ui.detail), detail);
    if(ui.desk_pending) {
        /* A tool page control runs as a short desk job; the screen stays as it is meanwhile. */
        if(state == BUDDY_IDLE || state == BUDDY_ERROR) {
            ui.desk_pending = false;
            if(state == BUDDY_ERROR) { ui.state = BUDDY_IDLE; page_notice("That didn’t work — try again"); }
        }
        refresh_status();
        return;
    }
    if((previous == BUDDY_IDLE || previous == BUDDY_ERROR) && state != BUDDY_IDLE && state != BUDDY_ERROR) {
        ui.reply_page = MODE_IDLE;   /* a new request: pages of earlier replies no longer apply */
    }
    if(ui.page_pinned && page_mode(ui.mode)) {
        /* A tool result's page outlasts the rest of its turn; speaking into the mic again ends it. */
        const bool new_request = !ui.voice_active &&
            (state == BUDDY_RECORDING || state == BUDDY_LISTENING || state == BUDDY_PREVIEW);
        if(!new_request) {
            ui.replying = false;
            ui.fresh_reply = false;
            if(ui.linger_timer) { lv_timer_del(ui.linger_timer); ui.linger_timer = NULL; }
            set_mode(ui.mode);   /* the rail's mic/stop follows the state */
            return;
        }
        ui.page_pinned = false;
    }
    if(ui.voice_active) {
        ui.replying = false;
        ui.fresh_reply = false;
        if(ui.mode != MODE_CHAT && ui.mode != MODE_SCENES && ui.mode != MODE_SETTINGS) set_mode(activity_mode());
        else refresh_status();
        return;
    }
    if(state == previous) {
        refresh_status();
        return;
    }
    ui.notice[0] = '\0';
    if(state == BUDDY_SPEAKING) {
        start_reply(true);
        return;
    }
    if(previous == BUDDY_SPEAKING && ui.replying) {
        /* Speech finished: show the whole reply, then return to idle. */
        refresh_speaking(true);
        end_reply_after(REPLY_LINGER_MS);
        refresh_status();
        return;
    }
    if(state == BUDDY_IDLE && ui.fresh_reply) {
        start_reply(false);
        return;
    }
    if(state != BUDDY_IDLE && state != BUDDY_ERROR) {
        ui.replying = false;
        set_mode(activity_mode());
        return;
    }
    if(ui.mode == MODE_LISTENING || ui.mode == MODE_THINKING || ui.mode == MODE_SPEAKING || state == BUDDY_ERROR) {
        ui.replying = false;
        set_mode(state == BUDDY_ERROR ? MODE_IDLE : rest_mode());
    }
    refresh_status();
}

void buddy_ui_set_voice(bool available, bool active, bool muted, bool connecting,
                        const char *role, const char *transcript)
{
    if(!ui.screen) return;
    const bool was_active = ui.voice_active;
    const bool caption_changed = strcmp(lv_label_get_text(ui.live_words), transcript ? transcript : "") != 0;
    const char *caption_role = role && strcmp(role, "user") == 0 ? "YOU" : "AI";
    if(ui.voice_live_available == available && ui.voice_active == active &&
       ui.voice_muted == muted && ui.voice_connecting == connecting &&
       !caption_changed && strcmp(lv_label_get_text(ui.live_role), caption_role) == 0) return;
    ui.voice_live_available = available;
    ui.voice_active = active;
    ui.voice_muted = muted;
    ui.voice_connecting = connecting;
    set_text(ui.live_role, caption_role);
    set_text(ui.live_words, transcript);
    if(active || was_active) {
        ui.replying = false;
        ui.fresh_reply = false;
        if(ui.linger_timer) { lv_timer_del(ui.linger_timer); ui.linger_timer = NULL; }
    }
    /* In live voice a pinned page stays until the user starts the next turn. */
    if(ui.page_pinned && (!active || (caption_changed && transcript && transcript[0] && strcmp(caption_role, "YOU") == 0))) {
        ui.page_pinned = false;
    }
    const bool keep = ui.mode == MODE_CHAT || ui.mode == MODE_SCENES || ui.mode == MODE_SETTINGS ||
        (ui.page_pinned && page_mode(ui.mode));
    set_mode(keep ? ui.mode : activity_mode());
    if(active && caption_changed && ui.prefs.captions) {
        lv_obj_update_layout(ui.live_scroll);
        const lv_coord_t visible = lv_obj_get_height(ui.live_scroll);
        const lv_coord_t written = lv_obj_get_height(ui.live_words);
        lv_obj_scroll_to_y(ui.live_scroll, written > visible ? written - visible : 0, LV_ANIM_OFF);
    }
}

void buddy_ui_set_network(bool connected, const char *ssid, const char *ip)
{
    /* Keep the current station address for physically approved browser setup. */
    if(!ui.screen) return;
    ui.connected = connected;
    copy(ui.ssid, sizeof(ui.ssid), ssid);
    copy(ui.station_ip, sizeof(ui.station_ip), connected ? ip : "");
    refresh_web_login();
    lv_obj_set_style_img_opa(ui.wifi, connected ? LV_OPA_COVER : 89, 0);
    refresh_status();
    if(ui.voice_active) refresh_live();
    if(ui.mode == MODE_SETTINGS) refresh_settings();
}

void buddy_ui_set_clock(bool ready, const char *local_time, int hour)
{
    if(!ui.screen) return;
    ui.clock_ready = ready && local_time && local_time[0];
    ui.hour = hour;
    set_text(ui.clock, ui.clock_ready ? local_time : "--:--");
    refresh_status();
}

void buddy_ui_set_messages(const buddy_ui_message_t *messages, size_t count)
{
    if(!ui.screen) return;
    if(!messages) count = 0;
    if(count > MAX_MESSAGES) {
        messages += count - MAX_MESSAGES;
        count = MAX_MESSAGES;
    }
    for(size_t i = 0; i < ui.message_count; ++i) {
        lv_mem_free(ui.message_text[i]);
        ui.message_text[i] = NULL;
    }
    size_t kept = 0;
    for(size_t i = 0; i < count; ++i) {
        const char *value = messages[i].text ? messages[i].text : "";
        const size_t length = strnlen(value, TEXT_MAX - 1);
        char *copy_text = lv_mem_alloc(length + 1);
        if(!copy_text) break;
        memcpy(copy_text, value, length);
        copy_text[length] = '\0';
        ui.message_text[kept] = copy_text;
        ui.message_ids[kept] = messages[i].id;
        ui.message_assistant[kept] = messages[i].assistant;
        copy(ui.message_time[kept], sizeof(ui.message_time[kept]), messages[i].time);
        const size_t tools = messages[i].tools ? LV_MIN(messages[i].tool_count, CHAT_CHIPS) : 0;
        for(size_t t = 0; t < tools; ++t) copy_tool(&ui.message_tools[kept][t], &messages[i].tools[t]);
        ui.message_tool_count[kept] = (uint8_t)tools;
        ++kept;
    }
    ui.message_count = kept;
    uint32_t reply_id = 0;
    if(last_message(true, &reply_id) && reply_id != ui.last_reply_id) {
        /* A first snapshot after boot is history, not a new reply. */
        if(ui.last_reply_id || ui.state != BUDDY_IDLE) ui.fresh_reply = true;
        ui.last_reply_id = reply_id;
    }
    if(!kept) ui.last_reply_id = 0;
    if(ui.mode == MODE_CHAT) {
        ui.chat_page = -1;
        refresh_chat();
    }
    if(ui.voice_active || (ui.page_pinned && page_mode(ui.mode))) ui.fresh_reply = false;
    else {
        if(ui.mode == MODE_SPEAKING) refresh_speaking(!ui.reply_spoken || ui.state != BUDDY_SPEAKING);
        if(ui.fresh_reply && ui.state == BUDDY_IDLE) start_reply(false);
        else if(ui.fresh_reply && ui.state == BUDDY_SPEAKING) ui.fresh_reply = false;
    }
    refresh_status();
}

void buddy_ui_set_tools(const buddy_ui_tool_t *tools, size_t count)
{
    if(!ui.screen) return;
    if(!tools) count = 0;
    /* Keep the latest calls; earlier ones have already scrolled out of the list. */
    const size_t first = count > THINK_ROWS ? count - THINK_ROWS : 0;
    ui.live_tool_count = count - first;
    for(size_t i = 0; i < ui.live_tool_count; ++i) copy_tool(&ui.live_tools[i], &tools[first + i]);
    /* Only a succeeded timer, notes, calculator or weather call opens its result
     * page; errors must not present a cached result as the requested effect. */
    size_t done = 0;
    ui_mode_t result_page = MODE_IDLE;
    for(size_t i = 0; i < count; ++i) {
        if(tools[i].state != BUDDY_TOOL_SUCCEEDED) continue;
        if(++done <= ui.tool_results_seen) continue;
        switch(tools[i].kind) {
            case BUDDY_TOOL_TIMER: result_page = MODE_TIMER; break;
            case BUDDY_TOOL_NOTES: result_page = MODE_NOTES; break;
            case BUDDY_TOOL_CALC: result_page = MODE_CALC; break;
            case BUDDY_TOOL_WEATHER: result_page = MODE_WEATHER; break;
            default: break;
        }
    }
    ui.tool_results_seen = done;   /* an empty list starts the next turn from zero */
    refresh_status();
    if(result_page == MODE_IDLE) return;
    ui.reply_page = result_page;
    if(ui.mode == MODE_CHAT || ui.mode == MODE_SCENES || ui.mode == MODE_SETTINGS) return;
    if(ui.mode == MODE_TIMER_DONE && result_page == MODE_TIMER) return;
    ui.replying = false;
    if(ui.linger_timer) { lv_timer_del(ui.linger_timer); ui.linger_timer = NULL; }
    set_mode(result_page);
    ui.page_pinned = true;
}

void buddy_ui_set_speech_pace(int words_per_minute)
{
    if(words_per_minute > 0) ui.wpm = words_per_minute;
}

void buddy_ui_set_asleep(bool asleep)
{
    if(!ui.screen) return;
    hidden(ui.wake_catcher, !asleep);
    if(asleep) {
        lv_timer_pause(ui.frame_timer);
        if(ui.mode == MODE_CHAT || ui.mode == MODE_SCENES || ui.mode == MODE_SETTINGS ||
           ui.mode == MODE_NOTES || ui.mode == MODE_CALC || ui.mode == MODE_WEATHER) set_mode(activity_mode());
    } else {
        lv_timer_resume(ui.frame_timer);
    }
}

void buddy_ui_set_timer(const buddy_ui_timer_t *timer)
{
    if(!ui.screen) return;
    const bool known = timer && timer->status != BUDDY_UI_TIMER_IDLE;
    ui.timer_known = known;
    if(known) {
        ui.timer = *timer;
        copy(ui.timer_text, sizeof(ui.timer_text), timer->label);
        ui.timer.label = ui.timer_text;
    }
    if(!ui.desk_pending) {
        /* Follow the timer: done shows its page, a cancelled or finished one closes it. */
        if(ui.mode == MODE_TIMER && known && ui.timer.status == BUDDY_UI_TIMER_DONE) {
            if(!timer_alert()) ui.timer_done_tick = lv_tick_get();
            set_mode(MODE_TIMER_DONE);
            return;
        }
        if((ui.mode == MODE_TIMER && !known) || (ui.mode == MODE_TIMER_DONE && (!known || ui.timer.status != BUDDY_UI_TIMER_DONE))) {
            set_mode(!known ? MODE_IDLE : MODE_TIMER);
            return;
        }
    }
    /* An expiry rings now unless something else holds the screen; rest_mode() picks it up later. */
    if(timer_alert() && (ui.mode == MODE_IDLE || page_mode(ui.mode)) && !ui.replying) {
        lv_disp_trig_activity(NULL);
        set_mode(MODE_TIMER_DONE);
        return;
    }
    if(ui.mode == MODE_TIMER || ui.mode == MODE_TIMER_DONE) refresh_page();
}

void buddy_ui_set_notes(const buddy_ui_note_t *notes, size_t count, bool ready)
{
    if(!ui.screen) return;
    if(!notes) count = 0;
    if(!ui.notes && count) ui.notes = psram_alloc(NOTE_ROWS * sizeof(note_slot_t));
    if(!ui.notes) count = 0;
    ui.notes_ready = ready;
    ui.note_count = LV_MIN(count, NOTE_ROWS);
    for(size_t i = 0; i < ui.note_count; ++i) {
        note_slot_t *n = &ui.notes[i];
        n->id = notes[i].id;
        n->task = notes[i].task;
        n->completed = notes[i].completed;
        n->urgent = notes[i].urgent;
        copy(n->due, sizeof(n->due), notes[i].due);
        copy(n->text, sizeof(n->text), notes[i].text);
        for(char *c = n->text; *c; ++c) if(*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    }
    if(ui.mode == MODE_NOTES) refresh_page();
}

void buddy_ui_set_calc(const buddy_ui_calc_t *calc)
{
    if(!ui.screen) return;
    ui.calc_known = calc != NULL;
    if(calc) {
        ui.calc_conversion = calc->conversion;
        copy(ui.calc_expression, sizeof(ui.calc_expression), calc->expression);
        ui.calc_input = calc->input;
        ui.calc_result = calc->value;
        ui.calc_from = (int8_t)(calc->conversion ? unit_index(calc->from) : -1);
        ui.calc_to = (int8_t)(calc->conversion ? unit_index(calc->to) : -1);
        if(ui.calc_from >= 0 && ui.calc_to >= 0 && UNITS[ui.calc_from].dimension != UNITS[ui.calc_to].dimension) ui.calc_to = -1;
        /* The answer's unit first, then the dimension's other units in their listed order. */
        size_t n = 0;
        ui.calc_choice[0] = ui.calc_choice[1] = ui.calc_choice[2] = -1;
        if(ui.calc_to >= 0) {
            ui.calc_choice[n++] = ui.calc_to;
            for(size_t i = 0; i < UNIT_COUNT && n < 3; ++i) {
                if(UNITS[i].dimension == UNITS[ui.calc_to].dimension && (int)i != ui.calc_from && (int)i != ui.calc_to) ui.calc_choice[n++] = (int8_t)i;
            }
        }
        ui.calc_shown = -1;
    }
    if(ui.mode == MODE_CALC) refresh_page();
}

void buddy_ui_set_weather(const buddy_ui_weather_t *weather)
{
    if(!ui.screen) return;
    ui.weather_known = weather != NULL;
    if(weather) {
        copy(ui.weather_location, sizeof(ui.weather_location), weather->location);
        copy(ui.weather_observed, sizeof(ui.weather_observed), weather->observed);
        copy(ui.weather_condition_text, sizeof(ui.weather_condition_text), weather->condition);
        ui.weather_code = weather->weather_code;
        ui.weather_degrees = weather->temperature_c;
        ui.weather_feels = weather->feels_like_c;
        ui.weather_high = weather->high_c;
        ui.weather_low = weather->low_c;
        ui.weather_hours = weather->forecast ? LV_MIN(weather->forecast_count, DESK_HOURS) : 0;
        for(size_t i = 0; i < ui.weather_hours; ++i) {
            copy(ui.weather_forecast[i].time, sizeof(ui.weather_forecast[i].time), weather->forecast[i].time);
            ui.weather_forecast[i].code = weather->forecast[i].weather_code;
            ui.weather_forecast[i].temp = weather->forecast[i].temperature_c;
            ui.weather_forecast[i].rain = weather->forecast[i].precipitation_probability;
        }
    }
    if(ui.mode == MODE_WEATHER) refresh_page();
}
