#include "local_time.h"
#include "test.h"

/* 2026-09-29 00:00:00 UTC */
#define MIDNIGHT_UTC 1790640000LL

static void test_minute_of_day(void)
{
    CHECK_EQ_INT(local_minute_of_day(MIDNIGHT_UTC, 0), 0);
    CHECK_EQ_INT(local_minute_of_day(MIDNIGHT_UTC, 480), 480);            /* 08:00 in UTC+8 */
    CHECK_EQ_INT(local_minute_of_day(MIDNIGHT_UTC + 20 * 3600, 480), 240); /* 04:00 next day */
    CHECK_EQ_INT(local_minute_of_day(MIDNIGHT_UTC, -210), 1440 - 210);    /* 20:30 day before */
    CHECK_EQ_INT(local_minute_of_day(MIDNIGHT_UTC + 59, 0), 0);
    CHECK_EQ_INT(local_minute_of_day(MIDNIGHT_UTC + 60, 0), 1);
}

static void test_window(void)
{
    /* 22:00-07:00, wrapping midnight */
    CHECK(!local_in_window(21 * 60 + 59, 22 * 60, 7 * 60));
    CHECK(local_in_window(22 * 60, 22 * 60, 7 * 60));
    CHECK(local_in_window(0, 22 * 60, 7 * 60));
    CHECK(local_in_window(6 * 60 + 59, 22 * 60, 7 * 60));
    CHECK(!local_in_window(7 * 60, 22 * 60, 7 * 60));
    /* 01:00-05:00, same day */
    CHECK(!local_in_window(0, 60, 300));
    CHECK(local_in_window(60, 60, 300));
    CHECK(!local_in_window(300, 60, 300));
    /* empty */
    CHECK(!local_in_window(100, 100, 100));
}

int main(void)
{
    RUN(test_minute_of_day);
    RUN(test_window);
    TEST_MAIN_END("local_time");
}
