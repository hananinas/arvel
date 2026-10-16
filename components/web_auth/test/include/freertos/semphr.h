#ifndef HOST_SEMPHR_H
#define HOST_SEMPHR_H
#include "freertos/FreeRTOS.h"
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{ return pthread_mutex_init(storage, NULL) == 0 ? storage : NULL; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, uint32_t timeout)
{ (void)timeout; return pthread_mutex_lock(mutex) == 0 ? pdTRUE : 0; }
static inline void xSemaphoreGive(SemaphoreHandle_t mutex) { pthread_mutex_unlock(mutex); }
#endif
