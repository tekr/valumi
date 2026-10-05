/*
 * The owner's settings, as one plain struct: type, defaults and validation
 * here, the JSON for the panel and for storage in settings_json.c -- both
 * pure, so host-tested -- and NVS storage in settings_store.c.
 *
 * Saved as JSON, so a field can be added or dropped freely: settings saved
 * before it keep everything else, and a new field starts at its default.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SET_MAX_NETS 4
#define SET_MAX_COINS 15
#define SET_SSID_MAX 32
#define SET_WIFI_PASS_MIN 8 /* WPA2 passphrase limits */
#define SET_WIFI_PASS_MAX 63
#define SET_INST_MAX 23
/* The symbol shares the title line with the price. */
#define SET_LABEL_MAX 4
#define SET_PANEL_PW_LEN 5
#define SET_DWELL_MIN_S 0 /* 0: a continuous scroll */
#define SET_DWELL_MAX_S 15
#define SET_BRIGHT_MIN 10
#define SET_BRIGHT_MAX 100
#define SET_FADE_MAX 100
#define SET_TRANSITION_MIN_MS 200
#define SET_TRANSITION_MAX_MS 3000
#define SET_STYLE_CROSSFADE 0
#define SET_STYLE_DIP 1
#define SET_CHART_CLOSE 0     /* a line through each candle's close */
#define SET_CHART_HIGHS_LOWS 1 /* a smooth line through the highs and lows of each swing */
#define SET_CHART_BAND 2      /* the close line over a band from low to high */
#define SET_NUM_RANGES 3
#define SET_TZ_MIN_MIN (-12 * 60)
#define SET_TZ_MAX_MIN (14 * 60)

typedef struct {
    char ssid[SET_SSID_MAX + 1];
    char password[SET_WIFI_PASS_MAX + 1]; /* empty = open network */
    bool static_ip;
    uint32_t ip, mask, gw, dns; /* host order; dns 0 = use the gateway */
} set_net_t;

typedef struct {
    char inst_id[SET_INST_MAX + 1]; /* OKX instId, e.g. "BTC-USDT" */
    char label[SET_LABEL_MAX + 1];
} set_coin_t;

typedef struct {
    set_net_t nets[SET_MAX_NETS]; /* most preferred first */
    int n_nets;

    set_coin_t coins[SET_MAX_COINS]; /* carousel order */
    int n_coins;

    int dwell_s;

    /* Kept as chosen even on a board without an accelerometer (where the
     * panel forces "fixed"), so the settings behave on one that has it. */
    bool orient_fixed;
    bool usb_left;

    int brightness;
    int transition_fade;  /* how far pages dim while changing, percent */
    int transition_style; /* SET_STYLE_* */
    int transition_ms;
    int range; /* chart range index */
    int chart_style; /* SET_CHART_* */

    bool night_on;
    int night_start_min; /* minutes after local midnight */
    int night_end_min;
    int night_brightness;

    int tz_offset_min; /* local = UTC + this */

    /* In the clear because the screen always shows it: that is how anyone
     * logs in. Generated, never chosen; upper case and digits so it reads
     * unambiguously. Empty only between settings_defaults() and the store
     * filling it. */
    char panel_pw[SET_PANEL_PW_LEN + 1];
} settings_t;

/** Factory defaults; panel_pw is left for the store, which has the RNG. */
void settings_defaults(settings_t *s);

bool settings_valid_inst_id(const char *inst_id);
/** Upper-case @p in into @p out and validate it as an instId. */
bool settings_parse_inst_id(const char *in, char out[SET_INST_MAX + 1]);
bool settings_valid_label(const char *label);
/** The base currency of an instId, cut to SET_LABEL_MAX. */
void settings_default_label(const char *inst_id, char *out, size_t n);

/** "a.b.c.d" -> host order. Leading zeros are refused: some parsers read
 * them as octal. */
bool settings_parse_ipv4(const char *str, uint32_t *out);
void settings_format_ipv4(uint32_t ip, char *out, size_t n);

/** What the store refuses to save, with the reason in @p err. */
bool settings_validate(const settings_t *s, char *err, size_t n);

#ifdef __cplusplus
}
#endif
