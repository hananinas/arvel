#pragma once

#include <stdint.h>

/* The QSPI panel stays portrait; only LVGL's logical coordinates change. */
enum {
    BUDDY_DISPLAY_WIDTH = 640,
    BUDDY_DISPLAY_HEIGHT = 172
};

/* Caller supplies a full logical frame and enough DMA space for `rows` physical
 * rows, with 0 <= physical_y and physical_y + rows <= BUDDY_DISPLAY_WIDTH.
 * Panel pixel (x,y) reads logical (y,171-x). No full-frame intermediate is required. */
static inline void buddy_display_rotate_strip(const uint16_t *logical, uint16_t *dma,
                                               int physical_y, int rows)
{
    /* Read adjacent pixels from PSRAM; scatter into fast internal DMA memory.
     * Reading whole columns first repeatedly evicts the source cache lines. */
    for(int physical_x = 0; physical_x < BUDDY_DISPLAY_HEIGHT; ++physical_x) {
        const uint16_t *source = logical +
            (BUDDY_DISPLAY_HEIGHT - 1 - physical_x) * BUDDY_DISPLAY_WIDTH + physical_y;
        for(int row = 0; row < rows; ++row) {
            dma[row * BUDDY_DISPLAY_HEIGHT + physical_x] = source[row];
        }
    }
}

/* Controller packed axes are long/short. Preserve the board's controller-to-
 * panel orientation before inverting the display transform. Saturation uses
 * inclusive pixel bounds, never the out-of-range width/height themselves. */
static inline void buddy_display_touch_to_logical(uint16_t controller_x, uint16_t controller_y,
                                                  uint16_t *logical_x, uint16_t *logical_y)
{
    if(controller_x >= BUDDY_DISPLAY_WIDTH) controller_x = BUDDY_DISPLAY_WIDTH - 1;
    if(controller_y >= BUDDY_DISPLAY_HEIGHT) controller_y = BUDDY_DISPLAY_HEIGHT - 1;
    const uint16_t physical_x = controller_y;
    const uint16_t physical_y = BUDDY_DISPLAY_WIDTH - 1 - controller_x;
    *logical_x = physical_y;
    *logical_y = BUDDY_DISPLAY_HEIGHT - 1 - physical_x;
}
