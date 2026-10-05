#include "auth_core.h"

#include <ctype.h>
#include <string.h>

void auth_core_init(auth_state_t *a)
{
    memset(a, 0, sizeof(*a));
}

bool auth_ct_equal(const uint8_t *x, const uint8_t *y, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= x[i] ^ y[i];
    }
    return diff == 0;
}

bool auth_password_ok(const char *given, const char *expected)
{
    size_t n = strlen(expected);
    if (n == 0 || strlen(given) != n) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(toupper((unsigned char)given[i]) ^ (unsigned char)expected[i]);
    }
    return diff == 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool auth_hex_decode(const char *hex, uint8_t out[AUTH_TOKEN_BYTES])
{
    if (hex == NULL || strlen(hex) != AUTH_TOKEN_HEX) {
        return false;
    }
    for (int i = 0; i < AUTH_TOKEN_BYTES; i++) {
        int hi = hexval(hex[2 * i]);
        int lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

void auth_hex_encode(const uint8_t in[AUTH_TOKEN_BYTES], char out[AUTH_TOKEN_HEX + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < AUTH_TOKEN_BYTES; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[AUTH_TOKEN_HEX] = '\0';
}

void auth_session_create(auth_state_t *a, const uint8_t rnd[AUTH_TOKEN_BYTES], int64_t now_us,
                         char out_hex[AUTH_TOKEN_HEX + 1])
{
    int slot = -1;
    for (int i = 0; i < AUTH_MAX_SESSIONS && slot < 0; i++) {
        if (!a->sessions[i].used) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < AUTH_MAX_SESSIONS; i++) {
            if (a->sessions[i].last_used_us < a->sessions[slot].last_used_us) {
                slot = i;
            }
        }
    }
    a->sessions[slot].used = true;
    memcpy(a->sessions[slot].id, rnd, AUTH_TOKEN_BYTES);
    a->sessions[slot].last_used_us = now_us;
    auth_hex_encode(rnd, out_hex);
}

static int find_session(const auth_state_t *a, const char *hex)
{
    uint8_t id[AUTH_TOKEN_BYTES];
    if (!auth_hex_decode(hex, id)) {
        return -1;
    }
    int found = -1;
    /* No early exit: the loop costs the same whichever slot matches. */
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        if (a->sessions[i].used && auth_ct_equal(a->sessions[i].id, id, AUTH_TOKEN_BYTES)) {
            found = i;
        }
    }
    return found;
}

bool auth_session_check(auth_state_t *a, const char *hex, int64_t now_us)
{
    int i = find_session(a, hex);
    if (i < 0) {
        return false;
    }
    if (now_us - a->sessions[i].last_used_us >= AUTH_SESSION_IDLE_US) {
        memset(&a->sessions[i], 0, sizeof(a->sessions[i]));
        return false;
    }
    a->sessions[i].last_used_us = now_us;
    return true;
}

void auth_session_drop(auth_state_t *a, const char *hex)
{
    int i = find_session(a, hex);
    if (i >= 0) {
        memset(&a->sessions[i], 0, sizeof(a->sessions[i]));
    }
}

void auth_qr_issue(auth_state_t *a, const uint8_t rnd[AUTH_TOKEN_BYTES], int64_t now_us,
                   int64_t ttl_us, char out_hex[AUTH_TOKEN_HEX + 1])
{
    memcpy(a->qr, rnd, AUTH_TOKEN_BYTES);
    a->qr_valid = true;
    a->qr_expires_us = now_us + ttl_us;
    auth_hex_encode(rnd, out_hex);
}

bool auth_qr_redeem(auth_state_t *a, const char *hex, int64_t now_us)
{
    uint8_t t[AUTH_TOKEN_BYTES];
    if (!a->qr_valid || !auth_hex_decode(hex, t)) {
        return false;
    }
    if (now_us >= a->qr_expires_us) {
        a->qr_valid = false;
        return false;
    }
    if (!auth_ct_equal(a->qr, t, AUTH_TOKEN_BYTES)) {
        return false;
    }
    a->qr_valid = false;
    memset(a->qr, 0, sizeof(a->qr));
    return true;
}

bool auth_login_allowed(const auth_state_t *a, int64_t now_us)
{
    return now_us >= a->locked_until_us;
}

void auth_login_result(auth_state_t *a, bool ok, int64_t now_us)
{
    if (ok) {
        a->fails = 0;
        return;
    }
    a->fails++;
    if (a->fails >= AUTH_MAX_FAILS) {
        /* Every failure from here on re-arms the lock, so guessing proceeds
         * at one attempt per 30 s for as long as it continues. */
        a->locked_until_us = now_us + AUTH_LOCKOUT_US;
    }
}

bool auth_cookie_value(const char *header, const char *name, char *out, size_t n)
{
    if (header == NULL) {
        return false;
    }
    size_t nl = strlen(name);
    const char *p = header;
    while (*p) {
        while (*p == ' ' || *p == ';') {
            p++;
        }
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > nl && strncmp(p, name, nl) == 0 && p[nl] == '=') {
            size_t vl = len - nl - 1;
            if (vl >= n) {
                return false;
            }
            memcpy(out, p + nl + 1, vl);
            out[vl] = '\0';
            return true;
        }
        if (!end) {
            break;
        }
        p = end + 1;
    }
    return false;
}
