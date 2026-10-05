#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "code.h"

void settings_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));

    static const app_coin_t coins[] = APP_COINS;
    for (size_t i = 0; i < sizeof(coins) / sizeof(coins[0]) && i < SET_MAX_COINS; i++) {
        strncpy(s->coins[i].inst_id, coins[i].inst_id, SET_INST_MAX);
        strncpy(s->coins[i].label, coins[i].label, SET_LABEL_MAX);
        s->n_coins++;
    }

    s->dwell_s = APP_ROTATE_INTERVAL_S;
    s->brightness = APP_BRIGHTNESS_DEFAULT;
    s->transition_fade = APP_TRANSITION_FADE;
    s->transition_style = SET_STYLE_CROSSFADE;
    s->chart_style = SET_CHART_CLOSE;
    s->transition_ms = APP_TRANSITION_MS;
    s->range = APP_DEFAULT_RANGE;
    s->night_start_min = APP_NIGHT_START_MIN;
    s->night_end_min = APP_NIGHT_END_MIN;
    s->night_brightness = APP_NIGHT_BRIGHTNESS;
    s->tz_offset_min = APP_TZ_OFFSET_MIN;
}

bool settings_parse_inst_id(const char *in, char out[SET_INST_MAX + 1])
{
    size_t n = strlen(in);
    if (n > SET_INST_MAX) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        out[i] = (char)toupper((unsigned char)in[i]);
    }
    return settings_valid_inst_id(out);
}

/* OKX instIds are upper-case alphanumerics joined by dashes: BTC-USDT,
 * BTC-USDT-SWAP. At least one dash, no empty parts. The characters matter
 * beyond tidiness: the id is pasted into a request path unescaped. */
bool settings_valid_inst_id(const char *id)
{
    size_t n = strlen(id);
    if (n < 3 || n > SET_INST_MAX) {
        return false;
    }
    int dashes = 0;
    char prev = '-';
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (c == '-') {
            if (prev == '-') {
                return false; /* leading or doubled dash */
            }
            dashes++;
        } else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
        prev = c;
    }
    return dashes >= 1 && prev != '-';
}

/* The large display font carries upper case and digits only. */
bool settings_valid_label(const char *label)
{
    size_t n = strlen(label);
    if (n < 1 || n > SET_LABEL_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = label[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

void settings_default_label(const char *inst_id, char *out, size_t n)
{
    size_t k = 0;
    while (inst_id[k] && inst_id[k] != '-' && k < SET_LABEL_MAX && k + 1 < n) {
        out[k] = inst_id[k];
        k++;
    }
    out[k] = '\0';
}

bool settings_parse_ipv4(const char *str, uint32_t *out)
{
    uint32_t ip = 0;
    const char *p = str;
    for (int part = 0; part < 4; part++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
        if (p[0] == '0' && isdigit((unsigned char)p[1])) {
            return false; /* leading zero */
        }
        int v = 0;
        int digits = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            p++;
            if (++digits > 3) {
                return false;
            }
        }
        if (v > 255) {
            return false;
        }
        ip = (ip << 8) | (uint32_t)v;
        if (part < 3) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return false;
    }
    *out = ip;
    return true;
}

void settings_format_ipv4(uint32_t ip, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xFF, (unsigned)(ip >> 16) & 0xFF,
             (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
}

/* A contiguous run of leading ones, not all zero. */
static bool valid_mask(uint32_t mask)
{
    if (mask == 0) {
        return false;
    }
    uint32_t inv = ~mask;
    return (inv & (inv + 1)) == 0; /* inv is 0...01...1 */
}

static bool valid_net(const set_net_t *n, char *err, size_t en)
{
    size_t sl = strlen(n->ssid);
    if (sl < 1 || sl > SET_SSID_MAX) {
        snprintf(err, en, "network name must be 1-%d characters", SET_SSID_MAX);
        return false;
    }
    size_t pl = strlen(n->password);
    if (pl != 0 && (pl < SET_WIFI_PASS_MIN || pl > SET_WIFI_PASS_MAX)) {
        snprintf(err, en, "password for '%s' must be %d-%d characters, or empty for an open "
                          "network", n->ssid, SET_WIFI_PASS_MIN, SET_WIFI_PASS_MAX);
        return false;
    }
    if (n->static_ip) {
        if (!valid_mask(n->mask)) {
            snprintf(err, en, "'%s': subnet mask is not valid", n->ssid);
            return false;
        }
        uint32_t host = n->ip & ~n->mask;
        if (n->ip == 0 || host == 0 || host == ~n->mask) {
            snprintf(err, en, "'%s': IP address is not a usable host address", n->ssid);
            return false;
        }
        if ((n->gw & n->mask) != (n->ip & n->mask) || n->gw == n->ip) {
            snprintf(err, en, "'%s': gateway must be another address on the same subnet",
                     n->ssid);
            return false;
        }
    }
    return true;
}

bool settings_validate(const settings_t *s, char *err, size_t en)
{
    if (s->n_nets < 0 || s->n_nets > SET_MAX_NETS) {
        snprintf(err, en, "at most %d networks", SET_MAX_NETS);
        return false;
    }
    for (int i = 0; i < s->n_nets; i++) {
        if (!valid_net(&s->nets[i], err, en)) {
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(s->nets[i].ssid, s->nets[j].ssid) == 0) {
                snprintf(err, en, "'%s' is listed twice", s->nets[i].ssid);
                return false;
            }
        }
    }
    if (s->n_coins < 1 || s->n_coins > SET_MAX_COINS) {
        snprintf(err, en, "between 1 and %d coins", SET_MAX_COINS);
        return false;
    }
    for (int i = 0; i < s->n_coins; i++) {
        if (!settings_valid_inst_id(s->coins[i].inst_id)) {
            snprintf(err, en, "'%s' is not a valid instrument id (like BTC-USDT)",
                     s->coins[i].inst_id);
            return false;
        }
        if (!settings_valid_label(s->coins[i].label)) {
            snprintf(err, en, "label for %s must be 1-%d characters of A-Z and 0-9",
                     s->coins[i].inst_id, SET_LABEL_MAX);
            return false;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(s->coins[i].inst_id, s->coins[j].inst_id) == 0) {
                snprintf(err, en, "%s is listed twice", s->coins[i].inst_id);
                return false;
            }
        }
    }
    if (s->dwell_s < SET_DWELL_MIN_S || s->dwell_s > SET_DWELL_MAX_S) {
        snprintf(err, en, "time per coin must be %d-%d s", SET_DWELL_MIN_S, SET_DWELL_MAX_S);
        return false;
    }
    if (s->brightness < SET_BRIGHT_MIN || s->brightness > SET_BRIGHT_MAX ||
        s->night_brightness < SET_BRIGHT_MIN || s->night_brightness > SET_BRIGHT_MAX) {
        snprintf(err, en, "brightness must be %d-%d%%", SET_BRIGHT_MIN, SET_BRIGHT_MAX);
        return false;
    }
    if (s->transition_fade < 0 || s->transition_fade > SET_FADE_MAX) {
        snprintf(err, en, "transition fade must be 0-%d%%", SET_FADE_MAX);
        return false;
    }
    if (s->transition_ms < SET_TRANSITION_MIN_MS || s->transition_ms > SET_TRANSITION_MAX_MS) {
        snprintf(err, en, "transition time must be %d-%d ms", SET_TRANSITION_MIN_MS,
                 SET_TRANSITION_MAX_MS);
        return false;
    }
    if (s->transition_style != SET_STYLE_CROSSFADE && s->transition_style != SET_STYLE_DIP) {
        snprintf(err, en, "unknown transition style");
        return false;
    }
    if (s->chart_style < SET_CHART_CLOSE || s->chart_style > SET_CHART_BAND) {
        snprintf(err, en, "unknown chart style");
        return false;
    }
    if (s->range < 0 || s->range >= SET_NUM_RANGES) {
        snprintf(err, en, "unknown chart range");
        return false;
    }
    if (s->night_start_min < 0 || s->night_start_min >= 1440 || s->night_end_min < 0 ||
        s->night_end_min >= 1440) {
        snprintf(err, en, "night times must be within the day");
        return false;
    }
    if (s->night_on && s->night_start_min == s->night_end_min) {
        snprintf(err, en, "night start and end must differ");
        return false;
    }
    if (!code_valid(s->panel_pw, SET_PANEL_PW_LEN)) {
        snprintf(err, en, "panel password is malformed");
        return false;
    }
    if (s->tz_offset_min < SET_TZ_MIN_MIN || s->tz_offset_min > SET_TZ_MAX_MIN ||
        s->tz_offset_min % 15 != 0) {
        snprintf(err, en, "time zone must be UTC-12:00 to UTC+14:00 in 15 minute steps");
        return false;
    }
    return true;
}
