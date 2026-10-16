#pragma once
#include <cstdint>
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr BaseType_t pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
