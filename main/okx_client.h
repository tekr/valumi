/*
 * Thin client for the OKX v5 public market-data REST API.
 *
 * Not thread-safe: one connection and one response buffer, intended to be
 * called from a single network task. The HTTPS connection is kept alive
 * across requests -- a TLS handshake per poll would dwarf the payloads.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float last;    /* latest trade price */
    float open24h; /* price 24 h ago; change = (last - open24h) / open24h */
} okx_ticker_t;

/** HTTP status of the most recent request: 200, 429, ...; 0 = transport
 * failure (DNS, TCP, TLS) before any status arrived. */
int okx_last_http_status(void);

/**
 * @brief Fetch an instrument's ticker.
 * @return ESP_OK, ESP_ERR_NOT_FOUND if OKX says there is no such instrument,
 *         or ESP_FAIL if the question could not be asked.
 */
esp_err_t okx_fetch_ticker(const char *inst_id, okx_ticker_t *out);

typedef struct {
    int64_t ts_ms; /* when the bar opened */
    float close, high, low;
} okx_candle_t;

/**
 * @brief Fetch candles, oldest first.
 *
 * @param max        capacity of @p out, and the request limit
 * @param before_ts  0, or only bars strictly newer than this (OKX treats
 *                   `before` as exclusive, so the bar you name is never resent)
 * @param after_ts   0, or only bars strictly older than this
 * @param out_n      bars written
 * @param out_confirmed_ts  opening ts of the newest CLOSED bar in this batch,
 *                   or 0 if the batch contained none -- which is the normal
 *                   case, since most fetches return only the forming bar
 *
 * Returns ESP_FAIL if any candle in the response is malformed: under an
 * incremental merge a partial batch would silently desynchronise the series.
 */
esp_err_t okx_fetch_candles(const char *inst_id, const char *bar, int max, int64_t before_ts,
                            int64_t after_ts, okx_candle_t *out, int *out_n,
                            int64_t *out_confirmed_ts);

#ifdef __cplusplus
}
#endif
