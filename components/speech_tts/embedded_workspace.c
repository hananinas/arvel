#include "embedded_workspace.h"
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void *espeak_workspace_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}
