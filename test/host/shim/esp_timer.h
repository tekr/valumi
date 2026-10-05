/* Host shim: a clock the tests set by hand. */
#pragma once
#include <stdint.h>
extern int64_t g_fake_now_us;
static inline int64_t esp_timer_get_time(void) { return g_fake_now_us; }
