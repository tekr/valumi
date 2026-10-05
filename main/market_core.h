/*
 * The arithmetic behind market.c: how a new coin list maps onto the coins
 * already held, when a price counts as stale, and how a batch of candles
 * folds into a series. Pure, tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "okx_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Plan the move from @p old_ids to @p new_ids.
 *
 * @param old_of_new out: for each new coin, the index of the same coin in the
 *                   old list (its charts carry over), or -1 for a new coin
 * @param new_of_old out: for each old coin, its index in the new list, or -1
 *                   if it is gone
 * @return true if nothing moved: same coins, same order
 */
bool coin_plan(const char *const *old_ids, int n_old, const char *const *new_ids, int n_new,
               int *old_of_new, int *new_of_old);

/**
 * @brief Seconds after which a price is drawn as stale.
 *
 * Only the coin on screen is polled, so a healthy price ages right round the
 * carousel (@p page_ms per coin, its slide in included) before its
 * pre-fetch; anything older than that plus a margin is a failure showing,
 * never the schedule. Never below @p floor_s.
 */
int coin_stale_after_s(int n_coins, int page_ms, int poll_s, int lead_s, int margin_s,
                       int floor_s);

typedef struct {
    /* Opening time in minutes since the epoch: every bar starts on a whole
     * minute, and this is half the size of the exchange's milliseconds. */
    int32_t ts_min;
    float close, high, low;
} candle_t;

/* One chart range's candles, oldest first, in storage the caller provides. */
typedef struct {
    candle_t *v;
    int cap;
    int n;
    int64_t confirmed_ts; /* ms; newest closed bar held, 0 = none yet */
} series_t;

/**
 * @brief Fold a fetched batch (oldest first) into @p s.
 *
 * Each bar replaces the one with the same opening time or, if newer than
 * everything held, is appended, dropping the oldest when full. Matching on
 * time rather than position means a batch that repeats bars, arrives twice,
 * or lacks a bar the exchange never published cannot misalign the series.
 */
void series_merge(series_t *s, const okx_candle_t *in, int n);

#ifdef __cplusplus
}
#endif
