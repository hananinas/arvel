#pragma once
#include "FreeRTOS.h"
struct HostTask;
using TaskHandle_t = HostTask *;
BaseType_t xTaskCreate(void (*entry)(void *), const char *, uint32_t, void *, unsigned, TaskHandle_t *);
void xTaskNotifyGive(TaskHandle_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
