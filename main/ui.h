/*
 * Rendering for the ticker. Functions of the state passed in: no fetching and
 * no timing -- main owns the loop and the transition engine, this file owns
 * the pixels. (One exception to "no state": ui_qr_make() parks its output
 * pointer in a static for the encoder's callback, so it is render-task only.)
 *
 * A frame is composed in layers so transitions can mix them:
 *
 *   ui_begin_frame(fb);                 // background
 *   ui_draw_page(fb, coin, alpha);      // page content, or ui_draw_page_rows()
 *   ui_draw_status(fb, net, pulse);     // fixed chrome, never animated
 *
 * page_transition.c composes the frames of a page change from these, drawing
 * both pages row by row.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_CHART_CLOSE,      /* a line through each candle's close */
    UI_CHART_HIGHS_LOWS, /* a smooth line through the highs and lows of each swing */
    UI_CHART_BAND,       /* the close line over a band from low to high */
} ui_chart_t;

typedef struct {
    char label[8];                    /* "BTC"; a copy, so it outlives a coin-list change */
    float last;                       /* 0 = no data yet */
    float open24h;
    /* The chart's candles, oldest first; the newest is still forming and
     * ends at the live price. */
    float closes[APP_CHART_MAX];
    float highs[APP_CHART_MAX];
    float lows[APP_CHART_MAX];
    int n_closes;
    ui_chart_t chart;
    bool stale;                       /* price too old to trust: see coin_stale_after_s() */
    float flash;                      /* 0..1: price-tick highlight, 1 = just changed */
    int range;                        /* active chart range index (labels the change group) */
    float h1_base;                    /* price a rolling hour ago; 0 = unknown */
} ui_coin_t;

typedef enum {
    UI_NET_CONNECTING, /* no Wi-Fi association: violet, breathing */
    UI_NET_DEGRADED,   /* Wi-Fi up but API fetches failing: amber */
    UI_NET_OK,         /* a quiet steel blue */
} ui_net_status_t;

/** Clear the frame to the background colour. */
void ui_begin_frame(uint16_t *fb);

/**
 * @brief Draw one coin's page content.
 *
 * @param alpha 255 = fully present; lower values fade every element toward
 *              the background
 */
void ui_draw_page(uint16_t *fb, const ui_coin_t *coin, uint8_t alpha);

/* A page is three rows -- the title, the change figures and the chart --
 * and a transition may move and fade each on its own. */
#define UI_PAGE_ROWS 3

typedef struct {
    int16_t dx, dy; /* shift in pixels */
    uint8_t alpha;  /* as for ui_draw_page() */
} ui_shift_t;

/** ui_draw_page() with each row placed separately. Drawing honours the clip
 * window of board_gfx_set_clip(), and rows that cannot show cost nothing. */
void ui_draw_page_rows(uint16_t *fb, const ui_coin_t *coin, const ui_shift_t rows[UI_PAGE_ROWS]);

/** Status dot, fixed top-right. Drawn solid regardless of transitions.
 * @p pulse (0..1) drives the disconnected state's slow breathe; pass 1 if
 * not animating. Ignored for the other states. */
void ui_draw_status(uint16_t *fb, ui_net_status_t net, float pulse);

/** Splash screen shown while Wi-Fi/SNTP come up. @p detail may be NULL. */
void ui_render_splash(uint16_t *fb, const char *detail);

/** Transient brightness indicator, drawn over the current frame. */
void ui_render_brightness_overlay(uint16_t *fb, int percent);

/* ---- Setup, address and login screens ----------------------------------- */

/* Version 10 is the largest the encoder is asked for: 57 modules a side,
 * far beyond the ~60 characters of the longest code shown. */
#define UI_QR_MAX 57

typedef struct {
    int size; /* modules per side; 0 = no code */
    uint8_t bits[(UI_QR_MAX * UI_QR_MAX + 7) / 8];
} ui_qr_t;

/** Encode @p text. Done once per screen, not per frame. */
bool ui_qr_make(ui_qr_t *out, const char *text);

/**
 * @brief A QR code on the left, a title and up to five lines on the right.
 *
 * Line prefixes: '*' a value that gets typed in (address, password), drawn
 * in the medium font when it fits; ' ' any other value, drawn bright; none, a
 * label, drawn muted. Otherwise the small font, or the compact built-in one
 * if a line is too wide -- a URL must never be clipped. Upper case only;
 * lower case is folded.
 */
void ui_render_qr_screen(uint16_t *fb, const ui_qr_t *qr, const char *title,
                         const char *const *lines, int n_lines);

typedef enum {
    UI_HOLD_LOGIN, /* 2-5 s */
    UI_HOLD_RESET, /* 5-10 s */
    UI_HOLD_RESET_ONLY, /* setup mode, 2-10 s: there is no login screen to offer */
} ui_hold_t;

/** The countdown banner shown while the button is held past 2 s. */
void ui_render_hold_overlay(uint16_t *fb, ui_hold_t phase, int secs_left);

#ifdef __cplusplus
}
#endif
