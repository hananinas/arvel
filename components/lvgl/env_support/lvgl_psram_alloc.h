#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

/* Display objects are not DMA buffers. Keep them out of the internal heap
 * needed by Wi-Fi and the Rust HTTPS worker's cache-safe pthread stack. */
static inline void *lvgl_psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void *lvgl_psram_realloc(void *pointer, size_t size)
{
    return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
