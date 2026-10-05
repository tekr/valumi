/*
 * Local time as the ticker needs it: a fixed UTC offset, and whether the
 * night-dimming window is open.
 *
 * A plain offset rather than a POSIX TZ string. The owner picks "UTC+8" from a
 * list, which is exactly an offset, and it avoids POSIX TZ's inverted sign --
 * UTC+8 is spelt "UTC-8" there -- which is a bug waiting to happen. The price
 * is no automatic daylight saving; the panel says so.
 *
 * Pure, tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Minutes after local midnight, 0..1439, for a Unix time and offset. */
int local_minute_of_day(int64_t unix_s, int tz_offset_min);

/**
 * @brief Is @p minute inside [start, end)? The window may wrap midnight
 * (22:00-07:00). start == end is an empty window.
 */
bool local_in_window(int minute, int start_min, int end_min);

#ifdef __cplusplus
}
#endif
