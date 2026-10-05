/*
 * Panel sessions, one-time QR login tokens, and login rate limiting.
 *
 * Pure: randomness and time are passed in, so every rule here -- single use,
 * expiry, eviction, lockout -- is tested on the host. auth.c wraps it with a
 * mutex, the hardware RNG, and the password hash.
 *
 * Everything lives in RAM. A reboot logs everyone out and voids any QR token
 * on screen, which is the right failure direction.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_TOKEN_BYTES 16
#define AUTH_TOKEN_HEX (AUTH_TOKEN_BYTES * 2)
#define AUTH_MAX_SESSIONS 4
/* A session unused this long is dropped. Matches the cookie's lifetime. */
#define AUTH_SESSION_IDLE_US (30LL * 24 * 3600 * 1000000)
#define AUTH_MAX_FAILS 5
#define AUTH_LOCKOUT_US (30LL * 1000000)

typedef struct {
    bool used;
    uint8_t id[AUTH_TOKEN_BYTES];
    int64_t last_used_us;
} auth_session_t;

typedef struct {
    auth_session_t sessions[AUTH_MAX_SESSIONS];
    bool qr_valid;
    uint8_t qr[AUTH_TOKEN_BYTES];
    int64_t qr_expires_us;
    int fails;              /* consecutive failed password attempts */
    int64_t locked_until_us;
} auth_state_t;

void auth_core_init(auth_state_t *a);

/** Length-independent-of-content comparison. */
bool auth_ct_equal(const uint8_t *x, const uint8_t *y, size_t n);

/** @p given matches @p expected, ignoring case: the password is upper case
 * on screen, and phone keyboards start in lower case. */
bool auth_password_ok(const char *given, const char *expected);

/** Lower-case hex of exactly AUTH_TOKEN_HEX digits -> bytes. */
bool auth_hex_decode(const char *hex, uint8_t out[AUTH_TOKEN_BYTES]);
void auth_hex_encode(const uint8_t in[AUTH_TOKEN_BYTES], char out[AUTH_TOKEN_HEX + 1]);

/**
 * @brief Start a session with id @p rnd; evicts the least recently used one
 * when full.
 */
void auth_session_create(auth_state_t *a, const uint8_t rnd[AUTH_TOKEN_BYTES], int64_t now_us,
                         char out_hex[AUTH_TOKEN_HEX + 1]);

/** True if @p hex names a live session; refreshes its LRU stamp. A session
 * idle for AUTH_SESSION_IDLE_US is dropped instead. */
bool auth_session_check(auth_state_t *a, const char *hex, int64_t now_us);

void auth_session_drop(auth_state_t *a, const char *hex);

/**
 * @brief Issue a fresh QR token, voiding any previous one.
 *
 * @param ttl_us how long it may be redeemed: the time it is on screen plus a
 *        little for the phone to open the link. A code nobody can see any more
 *        should not still work.
 */
void auth_qr_issue(auth_state_t *a, const uint8_t rnd[AUTH_TOKEN_BYTES], int64_t now_us,
                   int64_t ttl_us, char out_hex[AUTH_TOKEN_HEX + 1]);

/** Spend the QR token. True at most once per issue, and never after expiry. */
bool auth_qr_redeem(auth_state_t *a, const char *hex, int64_t now_us);

/** False while locked out after too many wrong passwords. */
bool auth_login_allowed(const auth_state_t *a, int64_t now_us);

void auth_login_result(auth_state_t *a, bool ok, int64_t now_us);

/**
 * @brief Pull the value of cookie @p name out of a Cookie header.
 * @return false if absent or too long for @p out.
 */
bool auth_cookie_value(const char *header, const char *name, char *out, size_t n);

#ifdef __cplusplus
}
#endif
