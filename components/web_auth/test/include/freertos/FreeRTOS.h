#ifndef HOST_FREERTOS_H
#define HOST_FREERTOS_H
#include <pthread.h>
#include <stdint.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mutex) pthread_mutex_lock(mutex)
#define portEXIT_CRITICAL(mutex) pthread_mutex_unlock(mutex)
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#endif
