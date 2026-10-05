/*
 * Captive-portal DNS for setup mode.
 *
 * While the ticker is its own hotspot, every name a phone looks up resolves
 * to the ticker. Phones probe a known URL on joining a network; when the probe
 * lands on the ticker instead, they open the setup page by themselves.
 *
 * captive_dns_reply() is the whole protocol and is pure (tested on the host);
 * captive_dns_start/stop run it on a UDP socket.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the answer to one DNS query.
 *
 * A queries (class IN) are answered with @p ip; every other type gets an
 * empty NOERROR answer, so a phone asking for AAAA falls back to A promptly
 * rather than waiting out a timeout.
 *
 * @param ip  host order, a.b.c.d == a << 24 | ...
 * @return reply length, or -1 to send nothing (not a query, malformed, or
 *         it would not fit in @p cap).
 */
int captive_dns_reply(const uint8_t *q, size_t qlen, uint8_t *out, size_t cap, uint32_t ip);

/** Start answering on port 53. Idempotent. */
esp_err_t captive_dns_start(uint32_t ip);

/** Stop answering and close the socket. Idempotent. */
void captive_dns_stop(void);

#ifdef __cplusplus
}
#endif
