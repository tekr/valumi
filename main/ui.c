#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "board_display.h"
#include "board_gfx.h"
#include "chart_path.h"
#include "logo.h"
#include "fonts.h"
#include "qrcode.h"

/* ---- Palette ------------------------------------------------------------ */
#define COL_BG board_rgb565(0, 0, 0)
#define COL_TEXT board_rgb565(235, 238, 245)
#define COL_MUTED board_rgb565(110, 120, 140)
#define COL_STAR board_rgb565(255, 200, 60)
#define COL_GRID board_rgb565(38, 38, 44)
#define COL_UP board_rgb565(22, 199, 132)
#define COL_DOWN board_rgb565(234, 57, 67)
#define COL_AMBER board_rgb565(245, 166, 35)
#define COL_WHITE board_rgb565(255, 255, 255)

/* ---- Layout (320x172, one coin per page) -------------------------------- */
#define MARGIN 14
#define TITLE_Y 12
#define CHANGE_Y 66
#define SPARK_Y 102
#define SPARK_H 58

#define DOT_CX 304
#define DOT_CY 18
#define DOT_R 5

/* Smallest move the highs-and-lows chart keeps, as a fraction of the chart's height:
 * about two pixels. */
#define CHART_MIN_SWING 0.04f
/* Opacity of the high-low band behind the line, 0-255. */
#define BAND_ALPHA 80
/* Peak opacity of the gradient fill under the sparkline, 0-255. */
#define FILL_ALPHA_TOP 135
#define FILL_ALPHA_TOP_DOWN 160 /* red needs a little more presence */
/* Up-fill: line colour lifted toward white. That trick fails for red --
 * white adds green, and red+green at low brightness reads as brown -- so the
 * down-fill is an explicit rose (red+blue only, no green contamination). */
#define FILL_LIGHTEN 70
#define COL_DOWN_FILL board_rgb565(255, 82, 95)

/* ---- Per-frame draw state (set by ui_draw_page) ------------------------- */
/* The page alpha and x offset thread through every helper; keeping them in
 * statics for the duration of one ui_draw_page call keeps the signatures
 * readable. Rendering stays single-threaded by design. */
static uint8_t s_alpha = 255;
static int s_xoff = 0;

/* Lerp a colour toward the background by the current page alpha, so a faded
 * page's elements sink into the backdrop rather than toward black. */
static uint16_t page_col(uint16_t c)
{
    if (s_alpha >= 255) {
        return c;
    }
    uint16_t bg = (uint16_t)((COL_BG >> 8) | (COL_BG << 8));
    uint16_t fg = (uint16_t)((c >> 8) | (c << 8));

    uint32_t br = (bg >> 11) & 0x1F, bgc = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    uint32_t fr = (fg >> 11) & 0x1F, fgc = (fg >> 5) & 0x3F, fbl = fg & 0x1F;

    uint32_t r = (fr * s_alpha + br * (255 - s_alpha)) / 255;
    uint32_t g = (fgc * s_alpha + bgc * (255 - s_alpha)) / 255;
    uint32_t b = (fbl * s_alpha + bb * (255 - s_alpha)) / 255;

    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return (uint16_t)((out >> 8) | (out << 8));
}

/* Blend two colours (both panel byte order), t in 0..255 toward b. */
static uint16_t mix_col(uint16_t a, uint16_t b, uint8_t t)
{
    uint16_t ua = (uint16_t)((a >> 8) | (a << 8));
    uint16_t ub = (uint16_t)((b >> 8) | (b << 8));

    uint32_t ar = (ua >> 11) & 0x1F, ag = (ua >> 5) & 0x3F, ab = ua & 0x1F;
    uint32_t br_ = (ub >> 11) & 0x1F, bg_ = (ub >> 5) & 0x3F, bb_ = ub & 0x1F;

    uint32_t r = (br_ * t + ar * (255 - t)) / 255;
    uint32_t g = (bg_ * t + ag * (255 - t)) / 255;
    uint32_t b2 = (bb_ * t + ab * (255 - t)) / 255;

    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b2);
    return (uint16_t)((out >> 8) | (out << 8));
}

/* ---- Formatting --------------------------------------------------------- */

/* "115234.7" -> "115,234"; keeps more decimals as magnitude drops so every
 * coin gets ~6 significant figures without overflowing the line. */
static void fmt_price(float v, char *out, size_t n)
{
    char raw[24];
    if (v >= 1000000.0f) {
        /* Seven digits with separators would collide with the status dot at
         * the large size; ungrouped they still clear it. */
        snprintf(out, n, "%.0f", (double)v);
        return;
    }
    if (v >= 10000.0f) {
        snprintf(raw, sizeof(raw), "%.0f", (double)v);
    } else if (v >= 1000.0f) {
        snprintf(raw, sizeof(raw), "%.1f", (double)v);
    } else if (v >= 10.0f) {
        snprintf(raw, sizeof(raw), "%.2f", (double)v);
    } else {
        snprintf(raw, sizeof(raw), "%.4f", (double)v);
    }

    const char *dot = strchr(raw, '.');
    int int_len = dot ? (int)(dot - raw) : (int)strlen(raw);
    int out_i = 0;
    for (int i = 0; i < int_len && out_i < (int)n - 1; i++) {
        if (i > 0 && (int_len - i) % 3 == 0) {
            out[out_i++] = ',';
        }
        out[out_i++] = raw[i];
    }
    for (const char *p = dot; p && *p && out_i < (int)n - 1; p++) {
        out[out_i++] = *p;
    }
    out[out_i] = '\0';
}

/* ---- Pieces ------------------------------------------------------------- */

/* Anti-aliased filled triangle, apex up or down, w wide, top-left at (x, y).
 * Rows are solid spans with the two edge pixels blended by their fractional
 * coverage, which is all the AA a 15-px glyph needs to sit beside the font. */
static void draw_triangle_aa(uint16_t *fb, int x, int y, int w, bool up, uint16_t color)
{
    int h = (w * 5 + 4) / 9; /* ~golden-ish ratio */
    float cx = (float)x + (float)w / 2.0f;

    for (int row = 0; row < h; row++) {
        float t = up ? ((float)row + 0.5f) / (float)h : ((float)(h - row) - 0.5f) / (float)h;
        float half = t * (float)w / 2.0f;
        float lf = cx - half;
        float rf = cx + half;

        int li = (int)ceilf(lf);
        int ri = (int)floorf(rf) - 1;
        if (ri >= li) {
            board_gfx_hline(fb, li, y + row, ri - li + 1, color);
        }
        board_gfx_blend_pixel(fb, li - 1, y + row, color,
                              (uint8_t)(255.0f * ((float)li - lf)));
        board_gfx_blend_pixel(fb, ri + 1, y + row, color,
                              (uint8_t)(255.0f * (rf - (float)(ri + 1))));
    }
}

/* @p a, n points spread across w columns, linearly interpolated at @p col. */
static float at_column(const float *a, int n, int col, int w)
{
    float fpos = (float)col * (float)(n - 1) / (float)(w - 1);
    int idx = (int)fpos;
    float frac = fpos - (float)idx;
    return idx + 1 < n ? a[idx] * (1.0f - frac) + a[idx + 1] * frac : a[idx];
}

static void widen(float *lo, float *hi, const float *v, int n)
{
    for (int i = 0; i < n; i++) {
        *lo = v[i] < *lo ? v[i] : *lo;
        *hi = v[i] > *hi ? v[i] : *hi;
    }
}

/* The line through @p v. Under it either a gradient fill or, given
 * @p band_lo and @p band_hi (one per point of v), a band between them. */
static void draw_sparkline(uint16_t *fb, int x, int y, int w, int h, const float *v, int n,
                           const float *band_lo, const float *band_hi, uint16_t line_color,
                           uint16_t fill_color, uint8_t fill_alpha)
{
    if (n < 2) {
        board_gfx_hline(fb, x, y + h / 2, w, page_col(COL_MUTED));
        return;
    }

    float lo = v[0], hi = v[0];
    widen(&lo, &hi, v, n);
    if (band_lo) {
        widen(&lo, &hi, band_lo, n);
        widen(&lo, &hi, band_hi, n);
    }
    float range = hi - lo;
    if (range <= 0.0f) {
        board_gfx_hline(fb, x, y + h / 2, w, line_color);
        return;
    }

    /* Faint dotted quartile lines give the eye a scale without an axis. */
    uint16_t grid = page_col(COL_GRID);
    for (int q = 0; q <= 4; q++) {
        int gy = y + (h * q) / 4;
        for (int gx = x; gx < x + w; gx += 7) {
            board_gfx_pixel(fb, gx, gy, grid);
        }
    }

    /* While the page is sliding (x offset nonzero), fill every other column:
     * motion hides the decimation completely, and the saved blends roughly
     * double the transition frame rate's headroom. */
    int col_step = (s_xoff != 0) ? 2 : 1;
    for (int col = 0; col < w; col += col_step) {
        if (band_lo) {
            int top = y + (h - 1) - (int)((at_column(band_hi, n, col, w) - lo) / range * (h - 1));
            int bot = y + (h - 1) - (int)((at_column(band_lo, n, col, w) - lo) / range * (h - 1));
            uint8_t a = (uint8_t)(BAND_ALPHA * s_alpha / 255);
            for (int py = top; py <= bot; py++) {
                board_gfx_blend_pixel(fb, x + col, py, fill_color, a);
            }
            continue;
        }
        float val = at_column(v, n, col, w);
        int cy = y + (h - 1) - (int)(((val - lo) / range) * (float)(h - 1));

        /* Gradient fill: line colour dissolving toward the bottom edge. */
        int depth = (y + h) - cy;
        for (int py = cy + 1; py < y + h; py++) {
            uint32_t a = (uint32_t)fill_alpha * (uint32_t)((y + h) - py) / (uint32_t)depth;
            /* Scale by page alpha so fades take the fill down with them. */
            a = a * s_alpha / 255;
            board_gfx_blend_pixel(fb, x + col, py, fill_color, (uint8_t)a);
        }
    }

    /* The line itself: 2-px stroke over the fill. */
    int px = -1, py = -1;
    for (int i = 0; i < n; i++) {
        int cx = x + (i * (w - 1)) / (n - 1);
        int cy = y + (h - 1) - (int)(((v[i] - lo) / range) * (float)(h - 1));
        if (px >= 0) {
            board_gfx_line(fb, px, py, cx, cy, line_color);
            board_gfx_line(fb, px, py + 1, cx, cy + 1, line_color);
        }
        px = cx;
        py = cy;
    }
    board_gfx_fill_circle(fb, px, py, 3, line_color);

}

/* A smooth line through the highs and lows of each swing, one value per
 * column of a chart @p w wide. */
static const float *extremes_line(const ui_coin_t *c, int w)
{
    static float columns[BOARD_LCD_V_RES];
    static chart_pt_t pts[2 * APP_CHART_MAX + 1], swings[2 * APP_CHART_MAX + 1];
    int n = chart_extremes(c->closes, c->highs, c->lows, c->n_closes, pts);
    n = chart_swings(pts, n, CHART_MIN_SWING, swings);
    chart_columns(swings, n, w, columns);
    return columns;
}

/* ---- Public ------------------------------------------------------------- */

void ui_begin_frame(uint16_t *fb)
{
    board_gfx_clear(fb, COL_BG);
}

void ui_draw_status(uint16_t *fb, ui_net_status_t net, float pulse)
{
    /* Deliberately outside the price palette: red/green mean money here.
     * Severity maps to presence, not just hue -- healthy is dim and steady,
     * degraded solid, disconnected breathes for salience. */
    uint16_t col;
    switch (net) {
    case UI_NET_OK:
        col = board_rgb565(56, 108, 180); /* quiet steel blue */
        break;
    case UI_NET_DEGRADED:
        col = COL_AMBER;
        break;
    default: {
        /* Violet, breathing between 45%% and full via the caller's pulse. */
        uint16_t violet = board_rgb565(178, 92, 255);
        uint8_t t = (uint8_t)(115.0f + 140.0f * pulse);
        col = mix_col(COL_BG, violet, t);
        break;
    }
    }
    board_gfx_fill_circle(fb, DOT_CX, DOT_CY, DOT_R + 2, COL_GRID);
    board_gfx_fill_circle(fb, DOT_CX, DOT_CY, DOT_R, col);
}

void ui_draw_page(uint16_t *fb, const ui_coin_t *c, int x_off, uint8_t alpha)
{
    s_alpha = alpha;
    s_xoff = x_off;
    int m = MARGIN + x_off;

    /* Title line: symbol in white, price in the day's colour, same size. */
    int pen = board_font_text(fb, m, TITLE_Y, c->label, &font_large, page_col(COL_TEXT));

    if (c->last <= 0.0f) {
        board_font_text(fb, pen + 14, TITLE_Y, "--", &font_large, page_col(COL_MUTED));
    } else {
        bool up_day = c->last >= c->open24h;
        uint16_t day_col = c->stale ? COL_MUTED : (up_day ? COL_UP : COL_DOWN);

        /* Tick flash: pull the price toward white as a fresh sample lands,
         * easing back to the trend colour over ~300 ms. */
        uint16_t price_col = day_col;
        if (c->flash > 0.0f && !c->stale) {
            price_col = mix_col(day_col, COL_WHITE, (uint8_t)(c->flash * 180.0f));
        }

        char price[20];
        fmt_price(c->last, price, sizeof(price));
        board_font_text(fb, pen + 14, TITLE_Y, price, &font_large, page_col(price_col));

        /* Left change group. Normally the exchange's rolling 24 h move; on
         * the 1D chart range that would duplicate the right group, so it
         * shows the last hour instead -- baseline is the previous hourly
         * candle's close (the newest is the live price itself). Both groups
         * always end at the same latest price the big number shows. */
        float base = c->open24h;
        const char *tag = "24H";
        if (c->range == 0) {
            /* True rolling hour from the 1m series; if that has not arrived
             * yet, the previous hourly candle close approximates it rather
             * than leaving the group blank. */
            if (c->h1_base > 0.0f) {
                base = c->h1_base;
                tag = "1H";
            } else if (c->n_closes >= 2) {
                base = c->closes[c->n_closes - 2];
                tag = "1H";
            }
        }
        if (base > 0.0f) {
            bool up_l = c->last >= base;
            uint16_t left_col = c->stale ? COL_MUTED : (up_l ? COL_UP : COL_DOWN);

            float pct = (c->last - base) / base * 100.0f;
            char pct_str[16];
            snprintf(pct_str, sizeof(pct_str), "%.2f%%", (double)fabsf(pct));

            draw_triangle_aa(fb, m, CHANGE_Y + (up_l ? 7 : 8), 15, up_l, page_col(left_col));
            int tx = board_font_text(fb, m + 24, CHANGE_Y, pct_str, &font_medium,
                                     page_col(left_col));
            board_font_text(fb, tx + 10, CHANGE_Y + 7, tag, &font_small, page_col(COL_MUTED));
        }

        /* Range change, right-aligned: measured against the active chart
         * range's oldest close, labelled to match -- 1D, 7D or 30D. Long
         * pressing the button cycles the range, and this group follows. */
        if (c->n_closes >= 2 && c->closes[0] > 0.0f) {
            static const app_range_t k_range_cfg[APP_NUM_RANGES] = APP_RANGES;
            const char *tag = k_range_cfg[c->range].tag;

            bool up_range = c->last >= c->closes[0];
            uint16_t range_col = c->stale ? COL_MUTED : (up_range ? COL_UP : COL_DOWN);

            float pctr = (c->last - c->closes[0]) / c->closes[0] * 100.0f;
            char pct_str[16];
            snprintf(pct_str, sizeof(pct_str), "%.2f%%", (double)fabsf(pctr));

            int pct_w = board_font_text_width(pct_str, &font_medium);
            int tag_w = board_font_text_width(tag, &font_small);
            int right = x_off + board_display_width() - MARGIN;
            int gx = right - tag_w - 10 - pct_w - 24;

            draw_triangle_aa(fb, gx, CHANGE_Y + (up_range ? 7 : 8), 15, up_range,
                             page_col(range_col));
            int tx = board_font_text(fb, gx + 24, CHANGE_Y, pct_str, &font_medium,
                                     page_col(range_col));
            board_font_text(fb, tx + 10, CHANGE_Y + 7, tag, &font_small, page_col(COL_MUTED));
        }
    }

    /* Sparkline coloured by its own span: first stored close vs the newest. */
    if (c->n_closes >= 2) {
        bool up_week = c->closes[c->n_closes - 1] >= c->closes[0];
        uint16_t line = c->stale ? COL_MUTED : (up_week ? COL_UP : COL_DOWN);
        uint16_t fill = up_week ? mix_col(line, COL_WHITE, FILL_LIGHTEN) : COL_DOWN_FILL;
        if (c->stale) {
            fill = mix_col(COL_MUTED, COL_WHITE, FILL_LIGHTEN);
        }
        uint8_t fill_a = up_week ? FILL_ALPHA_TOP : FILL_ALPHA_TOP_DOWN;
        const float *v = c->closes;
        int n = c->n_closes;
        if (c->chart == UI_CHART_HIGHS_LOWS) {
            v = extremes_line(c, board_display_width() - 2 * MARGIN);
            n = board_display_width() - 2 * MARGIN;
        }
        bool band = c->chart == UI_CHART_BAND;
        draw_sparkline(fb, m, SPARK_Y, board_display_width() - 2 * MARGIN, SPARK_H, v, n,
                       band ? c->lows : NULL, band ? c->highs : NULL, page_col(line),
                       page_col(fill), fill_a);
    } else {
        board_gfx_hline(fb, m, SPARK_Y + SPARK_H / 2, board_display_width() - 2 * MARGIN,
                        page_col(COL_GRID));
    }

    s_alpha = 255;
    s_xoff = 0;
}

static void draw_mask(uint16_t *fb, int x0, int y0, const uint8_t *mask, uint16_t color)
{
    for (int y = 0; y < logo_height; y++) {
        for (int x = 0; x < logo_width; x++) {
            int p = y * logo_width + x;
            uint8_t a = (p & 1) ? mask[p >> 1] & 0x0F : mask[p >> 1] >> 4;
            if (a) {
                board_gfx_blend_pixel(fb, x0 + x, y0 + y, color, (uint8_t)(a * 17));
            }
        }
    }
}

void ui_render_splash(uint16_t *fb, const char *detail)
{
    board_gfx_clear(fb, COL_BG);

    /* The logo and the status line under it, centred as one block. */
    const int gap = 22;
    int y = (board_display_height() - (logo_height + gap + font_small.line_height)) / 2;
    int x = (board_display_width() - logo_width) / 2;
    draw_mask(fb, x, y, logo_text, COL_TEXT);
    draw_mask(fb, x, y, logo_star, COL_STAR);

    if (detail) {
        int dw = board_font_text_width(detail, &font_small);
        board_font_text(fb, (board_display_width() - dw) / 2, y + logo_height + gap, detail,
                        &font_small, COL_MUTED);
    }
}

void ui_render_brightness_overlay(uint16_t *fb, int percent)
{
    char label[20];
    snprintf(label, sizeof(label), "BRIGHT %d%%", percent);

    int w = board_font_text_width(label, &font_small) + 16;
    int h = font_small.line_height + 8;
    int x = board_display_width() - w - 8;
    int y = board_display_height() - h - 8;

    board_gfx_fill_rect(fb, x, y, w, h, COL_GRID);
    board_gfx_rect(fb, x, y, w, h, COL_MUTED);
    board_font_text(fb, x + 8, y + 4, label, &font_small, COL_TEXT);
}

/* ---- Setup, address and login screens ----------------------------------- */

/* esp_qrcode_generate() reports through a callback with no context pointer,
 * so the destination is parked here for the duration of one call. Only the
 * render task makes QR codes. */
static ui_qr_t *s_qr_target;

static void qr_capture(esp_qrcode_handle_t q)
{
    int n = esp_qrcode_get_size(q);
    if (n > UI_QR_MAX) {
        s_qr_target->size = 0;
        return;
    }
    memset(s_qr_target->bits, 0, sizeof(s_qr_target->bits));
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            if (esp_qrcode_get_module(q, x, y)) {
                int i = y * n + x;
                s_qr_target->bits[i / 8] |= (uint8_t)(1u << (i % 8));
            }
        }
    }
    s_qr_target->size = n;
}

bool ui_qr_make(ui_qr_t *out, const char *text)
{
    out->size = 0;
    s_qr_target = out;
    esp_qrcode_config_t cfg = {
        .display_func = qr_capture,
        .max_qrcode_version = 10,
        /* Medium: a phone reads it from a glossy screen at an angle. */
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };
    esp_err_t err = esp_qrcode_generate(&cfg, text);
    s_qr_target = NULL;
    return err == ESP_OK && out->size > 0;
}

static bool qr_module(const ui_qr_t *q, int x, int y)
{
    int i = y * q->size + x;
    return (q->bits[i / 8] >> (i % 8)) & 1;
}

/* Text in the small font if it fits @p max_w, else the built-in 5x7 font,
 * which fits ~40% more characters. Returns the height used. */
static int fit_text(uint16_t *fb, int x, int y, const char *str, int max_w, uint16_t col)
{
    if (board_font_text_width(str, &font_small) <= max_w) {
        board_font_text(fb, x, y, str, &font_small, col);
        return font_small.line_height;
    }
    board_gfx_text(fb, x, y + 3, str, col, 1);
    return BOARD_GFX_GLYPH_H + 6;
}

void ui_render_qr_screen(uint16_t *fb, const ui_qr_t *qr, const char *title,
                         const char *const *lines, int n_lines)
{
    board_gfx_clear(fb, COL_BG);
    const int h = board_display_height();
    const int w = board_display_width();

    /* The code on a white card with a quiet zone of four modules, as the
     * standard asks -- phones read a dark-on-light code on a black screen far
     * more reliably with the white margin than without. */
    int text_x = MARGIN;
    if (qr && qr->size > 0) {
        int total = qr->size + 8;
        /* 3 px a module: ~12 mm across on this panel, which a phone reads
         * without trouble, and it leaves the text column wide enough for an
         * IP address in the medium font. */
        int px = (h - 8) / total;
        if (px > 3) {
            px = 3;
        }
        if (px < 1) {
            px = 1;
        }
        int card = total * px;
        int cx = 6;
        int cy = (h - card) / 2;
        board_gfx_fill_rect(fb, cx, cy, card, card, COL_WHITE);
        for (int y = 0; y < qr->size; y++) {
            for (int x = 0; x < qr->size; x++) {
                if (qr_module(qr, x, y)) {
                    board_gfx_fill_rect(fb, cx + (x + 4) * px, cy + (y + 4) * px, px, px, COL_BG);
                }
            }
        }
        text_x = cx + card + 12;
    }

    int col_w = w - text_x - 6;
    int y = 10;
    if (title) {
        if (board_font_text_width(title, &font_medium) <= col_w) {
            board_font_text(fb, text_x, y, title, &font_medium, COL_TEXT);
            y += font_medium.line_height + 6;
        } else {
            y += fit_text(fb, text_x, y, title, col_w, COL_TEXT) + 6;
        }
    }
    for (int i = 0; i < n_lines && y < h - 8; i++) {
        const char *l = lines[i];
        if (l == NULL || l[0] == '\0') {
            y += 8; /* a blank line is a small gap, not a whole line */
            continue;
        }
        /* '*' starts a value that gets typed in -- an address, a password --
         * drawn in the medium font when it fits; ' ' starts any other value,
         * drawn bright; the rest are labels, drawn muted. */
        if (l[0] == '*' && board_font_text_width(l + 1, &font_medium) <= col_w) {
            board_font_text(fb, text_x, y, l + 1, &font_medium, COL_TEXT);
            y += font_medium.line_height + 4;
            continue;
        }
        bool value = l[0] == ' ' || l[0] == '*';
        y += fit_text(fb, text_x, y, value ? l + 1 : l, col_w, value ? COL_TEXT : COL_MUTED) + 4;
    }
}

void ui_render_hold_overlay(uint16_t *fb, ui_hold_t phase, int secs_left)
{
    const int w = board_display_width();
    const int h = board_display_height();
    const int bh = 58;
    const int y = h - bh - 6;

    board_gfx_fill_rect(fb, 6, y, w - 12, bh, COL_GRID);
    bool reset = phase != UI_HOLD_LOGIN;
    board_gfx_rect(fb, 6, y, w - 12, bh, reset ? COL_DOWN : COL_MUTED);

    char num[8];
    snprintf(num, sizeof(num), "%d", secs_left);
    int nw = board_font_text_width(num, &font_large);
    uint16_t num_col = reset ? COL_DOWN : COL_AMBER;
    board_font_text(fb, w - 18 - nw, y + (bh - font_large.line_height) / 2, num, &font_large,
                    num_col);

    int tx = 16;
    int col_w = w - 18 - nw - 12 - tx;
    if (phase == UI_HOLD_LOGIN) {
        fit_text(fb, tx, y + 10, "KEEP HOLDING FOR", col_w, COL_TEXT);
        fit_text(fb, tx, y + 30, "LOGIN DETAILS", col_w, COL_TEXT);
    } else if (phase == UI_HOLD_RESET_ONLY) {
        fit_text(fb, tx, y + 10, "KEEP HOLDING FOR", col_w, COL_TEXT);
        fit_text(fb, tx, y + 30, "FACTORY RESET", col_w, COL_DOWN);
    } else {
        fit_text(fb, tx, y + 10, "RELEASE NOW FOR LOGIN DETAILS", col_w, COL_TEXT);
        fit_text(fb, tx, y + 30, "KEEP HOLDING FOR FACTORY RESET", col_w, COL_DOWN);
    }
}
