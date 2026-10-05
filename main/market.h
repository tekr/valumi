/*
 * Prices and charts for the owner's coins, kept fresh by a task of its own
 * that owns the exchange connection.
 *
 * Only the coin on screen is polled, plus the next one just before it
 * arrives; charts fill the gaps between polls, one request at a time. The
 * coin list follows the settings, changed on the market task between
 * requests, so a fetch in flight never lands in a coin that has just moved
 * or gone.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MARKET_COIN_OK,          /* the exchange knows it */
    MARKET_COIN_UNKNOWN,     /* the exchange says there is no such instrument */
    MARKET_COIN_UNREACHABLE, /* could not ask: offline, busy, or the exchange is down */
} market_check_t;

/** Take the coin list from the settings and start the task. Needs
 * settings_store_init() and carousel_init() first. */
void market_start(void);

/**
 * @brief Copy what the renderer needs, with the charts for @p range.
 * @param gen out: changes whenever the list does, so per-index state can be
 *            dropped
 * @return the number of coins
 */
int market_snapshot(ui_coin_t *out, int range, uint32_t *gen);

/** Where startup has got to, for the splash: upper case, digits, ": / - . %". */
void market_progress(char *out, size_t n);

bool market_clock_synced(void);

/** Whether the last price poll succeeded. */
bool market_fetch_ok(void);

/** Ask the exchange about @p inst_id, waiting at most @p timeout_ms. Any task:
 * the question is answered on the market task. */
market_check_t market_check_coin(const char *inst_id, float *price, int timeout_ms);

#ifdef __cplusplus
}
#endif
