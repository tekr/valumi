#include "local_time.h"

#include <stdlib.h>

int local_minute_of_day(int64_t unix_s, int tz_offset_min)
{
    int64_t local_min = unix_s / 60 + tz_offset_min;
    int64_t m = local_min % 1440;
    return (int)(m < 0 ? m + 1440 : m);
}

bool local_in_window(int minute, int start_min, int end_min)
{
    if (start_min == end_min) {
        return false;
    }
    if (start_min < end_min) {
        return minute >= start_min && minute < end_min;
    }
    return minute >= start_min || minute < end_min; /* wraps midnight */
}
