#include "market_core.h"
#include "test.h"

static void test_coin_plan(void)
{
    const char *old_ids[] = {"OKB-USDT", "BTC-USDT", "ETH-USDT", "SOL-USDT"};
    int map[8], back[8];

    /* Unchanged. */
    CHECK(coin_plan(old_ids, 4, old_ids, 4, map, back));
    CHECK_EQ_INT(back[2], 2);

    /* Reordered, one removed, one added. */
    const char *new_ids[] = {"ETH-USDT", "DOGE-USDT", "BTC-USDT"};
    CHECK(!coin_plan(old_ids, 4, new_ids, 3, map, back));
    CHECK_EQ_INT(map[0], 2);  /* ETH keeps its charts */
    CHECK_EQ_INT(map[1], -1); /* DOGE is new */
    CHECK_EQ_INT(map[2], 1);
    CHECK_EQ_INT(back[0], -1); /* OKB is gone */
    CHECK_EQ_INT(back[1], 2);  /* BTC moved to the end */
    CHECK_EQ_INT(back[2], 0);  /* ETH to the front */
    CHECK_EQ_INT(back[3], -1);

    /* Same set, one fewer: not "same". */
    CHECK(!coin_plan(old_ids, 4, old_ids, 3, map, back));
}

static void test_stale_threshold(void)
{
    /* Four coins at 8 s: 3*8 + 3 - 1 + 4 = 30, the old fixed value. */
    CHECK_EQ_INT(coin_stale_after_s(4, 8680, 3, 1, 4, 30), 33);
    /* One coin: the floor. */
    CHECK_EQ_INT(coin_stale_after_s(1, 8680, 3, 1, 4, 30), 30);
    /* Fifteen, the most allowed, at the longest dwell and slide (15 s + 3 s). */
    CHECK_EQ_INT(coin_stale_after_s(15, 18000, 3, 1, 4, 30), 258);
    /* A continuous scroll: fifteen coins sliding for 3 s each, no dwell. The
     * slide time alone carries a price 42 s round the carousel. */
    CHECK_EQ_INT(coin_stale_after_s(15, 3000, 3, 1, 4, 30), 48);
}

#define MIN_MS 60000LL

/* Merge bars at the given minutes, each closing at its minute number, with
 * its high one above and its low one below. */
static void merge(series_t *s, const int *minutes, int n)
{
    okx_candle_t in[8];
    for (int i = 0; i < n; i++) {
        float c = (float)minutes[i];
        in[i] = (okx_candle_t){minutes[i] * MIN_MS, c, c + 1, c - 1};
    }
    series_merge(s, in, n);
}

static void test_series_merge(void)
{
    candle_t store[4];
    series_t s = {.v = store, .cap = 4};
    merge(&s, (const int[]){10, 11, 12}, 3);
    CHECK_EQ_INT(s.n, 3);
    CHECK_EQ_INT(s.v[0].ts_min, 10);
    CHECK(s.v[0].high == 11.0f && s.v[0].low == 9.0f);

    /* The forming bar updates in place, high and low included; a repeat
     * changes nothing. */
    okx_candle_t forming = {12 * MIN_MS, 99.0f, 120.0f, 5.0f};
    series_merge(&s, &forming, 1);
    CHECK_EQ_INT(s.n, 3);
    CHECK(s.v[2].close == 99.0f && s.v[2].high == 120.0f && s.v[2].low == 5.0f);
    merge(&s, (const int[]){11, 12}, 2);
    CHECK_EQ_INT(s.n, 3);
    CHECK(s.v[2].close == 12.0f);

    /* A gap the exchange never filled stays a gap: times, not positions. */
    merge(&s, (const int[]){14}, 1);
    CHECK_EQ_INT(s.n, 4);
    CHECK_EQ_INT(s.v[3].ts_min, 14);

    /* Full: the oldest goes. */
    merge(&s, (const int[]){15, 16}, 2);
    CHECK_EQ_INT(s.n, 4);
    CHECK_EQ_INT(s.v[0].ts_min, 12);
    CHECK_EQ_INT(s.v[3].ts_min, 16);

    /* Older than the window and not held: ignored. */
    merge(&s, (const int[]){5, 13}, 2);
    CHECK_EQ_INT(s.n, 4);
    CHECK_EQ_INT(s.v[0].ts_min, 12);
    for (int i = 1; i < s.n; i++) {
        CHECK(s.v[i].ts_min > s.v[i - 1].ts_min);
    }
}

int main(void)
{
    RUN(test_coin_plan);
    RUN(test_stale_threshold);
    RUN(test_series_merge);
    TEST_MAIN_END("market_core");
}
