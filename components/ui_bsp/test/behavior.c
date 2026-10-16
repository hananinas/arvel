/* Actual UI/LVGL/assets; only the display is replaced, never a tool execution. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "buddy_ui.h"
#include "lvgl.h"
static lv_color_t drawing[640 * 172];
static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    (void)pixels;
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < 640 && area->y2 < 172);
    lv_disp_flush_ready(driver);
}
static bool visible_text(lv_obj_t *object, const char *text)
{
    if(lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    if(lv_obj_check_type(object, &lv_label_class) && strstr(lv_label_get_text(object), text)) {
        lv_area_t bounds;
        lv_obj_get_coords(object, &bounds);
        bool visible = bounds.x1 >= 0 && bounds.y1 >= 0 && bounds.x2 < 640 && bounds.y2 < 172;
        for(lv_obj_t *parent = lv_obj_get_parent(object); visible && parent; parent = lv_obj_get_parent(parent)) {
            if(lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) visible = false;
            if(lv_obj_has_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) continue;
            lv_area_t clip;
            lv_obj_get_coords(parent, &clip);
            visible = visible && bounds.x1 >= clip.x1 && bounds.y1 >= clip.y1 &&
                      bounds.x2 <= clip.x2 && bounds.y2 <= clip.y2;
        }
        if(visible) return true;
    }
    for(uint32_t i = 0; i < lv_obj_get_child_cnt(object); ++i)
        if(visible_text(lv_obj_get_child(object, i), text)) return true;
    return false;
}
int main(void)
{
    lv_init();
    lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, drawing, NULL, 640 * 172);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 640; driver.ver_res = 172;
    driver.draw_buf = &buffer; driver.flush_cb = flush;
    lv_disp_t *display = lv_disp_drv_register(&driver);
    assert(display);
    buddy_ui_init(NULL);
    buddy_ui_set_network(true, "Isolated UI fixture", "127.0.0.1");
    buddy_ui_set_status(BUDDY_THINKING, "Isolated UI fixture", true);
    char call_id[129]; memset(call_id, 'x', sizeof(call_id) - 1); call_id[128] = 0;
    buddy_ui_tool_t failed = {.kind = BUDDY_TOOL_MCP, .verb = "Niko",
        .target = "00000000-0000-4000-8000-000000000001", .state = BUDDY_TOOL_FAILED,
        .id = call_id, .error = "offline"};
    buddy_ui_set_tools(&failed, 1);
    lv_refr_now(display);
    /* Even maximum provider identity/long target must not erase the domain error. */
    assert(visible_text(lv_scr_act(), failed.error));
    buddy_ui_set_tools(NULL, 0);
    buddy_ui_calc_t cached = {.expression = "stale fixture result", .input = 42.5, .value = 42.5};
    buddy_ui_set_calc(&cached);
    buddy_ui_set_status(BUDDY_THINKING, "Isolated UI fixture", true);
    buddy_ui_tool_t uncertain = {.kind = BUDDY_TOOL_CALC, .verb = "Calc",
        .state = BUDDY_TOOL_UNKNOWN, .id = call_id, .error = "unconfirmed"};
    buddy_ui_set_tools(&uncertain, 1);
    lv_refr_now(display);
    /* An uncertain call cannot open a page showing the previous successful value. */
    assert(visible_text(lv_scr_act(), uncertain.error));
    assert(!visible_text(lv_scr_act(), "42.5"));
    puts("native UI: domain errors survive long identities; unknown calls do not show cached success");
    return 0;
}
