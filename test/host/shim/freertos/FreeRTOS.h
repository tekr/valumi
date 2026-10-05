/* Host shim: the tests are single-threaded, so a mutex is a no-op. */
#pragma once
#include <stdint.h>
#define portMAX_DELAY 0xFFFFFFFFu
typedef uint32_t TickType_t;
