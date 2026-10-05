#include "wifi_qr.h"

#include <string.h>

typedef struct {
    char *out;
    size_t n;
    size_t len;
    bool ok;
} sb_t;

static void put(sb_t *b, char c)
{
    if (b->len + 1 >= b->n) {
        b->ok = false;
        return;
    }
    b->out[b->len++] = c;
    b->out[b->len] = '\0';
}

static void put_str(sb_t *b, const char *s)
{
    while (*s) {
        put(b, *s++);
    }
}

static void put_escaped(sb_t *b, const char *s)
{
    for (; *s; s++) {
        if (strchr("\\;,:\"", *s)) {
            put(b, '\\');
        }
        put(b, *s);
    }
}

bool wifi_qr_join_text(const char *ssid, const char *password, char *out, size_t n)
{
    if (n == 0) {
        return false;
    }
    sb_t b = {.out = out, .n = n, .len = 0, .ok = true};
    out[0] = '\0';
    bool open = password == NULL || password[0] == '\0';
    put_str(&b, open ? "WIFI:T:nopass;S:" : "WIFI:T:WPA;S:");
    put_escaped(&b, ssid);
    if (!open) {
        put_str(&b, ";P:");
        put_escaped(&b, password);
    }
    put_str(&b, ";;");
    return b.ok;
}
