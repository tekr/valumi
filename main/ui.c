#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
/* The alpha and offset of the row being drawn thread through every helper;
 * keeping them in statics for the duration of one ui_draw_page_rows call
 * keeps the signatures readable. Rendering stays single-threaded by design. */
static uint8_t s_alpha = 255;
static int s_xoff = 0;
static int s_yoff = 0;

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
        if (ri < li - 1) {
            /* The apex: the whole row falls inside one pixel. */
            board_gfx_blend_pixel(fb, li - 1, y + row, color, (uint8_t)(255.0f * (rf - lf)));
            continue;
        }
        if (ri >= li) {
            board_gfx_hline(fb, li, y + row, ri - li + 1, color);
        }
        board_gfx_blend_pixel(fb, li - 1, y + row, color,
                              (uint8_t)(255.0f * ((float)li - lf)));
        board_gfx_blend_pixel(fb, ri + 1, y + row, color,
                              (uint8_t)(255.0f * (rf - (float)(ri + 1))));
    }
}

static void widen(float *lo, float *hi, const float *v, int n)
{
    for (int i = 0; i < n; i++) {
        *lo = v[i] < *lo ? v[i] : *lo;
        *hi = v[i] > *hi ? v[i] : *hi;
    }
}

/* ---- The chart line ----------------------------------------------------- */

/* The chart is most of the pixels a frame blends, so its fills write the
 * frame buffer directly -- the caller keeps inside the clip window -- with a
 * colour unpacked once and an opacity of 0..256, which blends with shifts. */
typedef struct {
    int r, g, b;
} rgb_t;

static rgb_t unpack(uint16_t panel_col)
{
    uint16_t c = (uint16_t)((panel_col >> 8) | (panel_col << 8));
    return (rgb_t){(c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F};
}

static inline void blend_into(uint16_t *p, rgb_t fg, int a256)
{
    uint16_t bg = (uint16_t)((*p >> 8) | (*p << 8));
    int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
    r += ((fg.r - r) * a256) >> 8;
    g += ((fg.g - g) * a256) >> 8;
    b += ((fg.b - b) * a256) >> 8;
    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    *p = (uint16_t)((out >> 8) | (out << 8));
}

/* Positions along the line are kept in 1/32 px, so the stroke's edges land
 * between pixels and can be blended by how much of each pixel they cover. */
#define SQ 32
#define STROKE_HALF SQ /* half the line's width: a 2 px stroke */
#define STROKE_SUB 2   /* samples across each pixel column */
#define STROKE_WIN (STROKE_HALF * STROKE_SUB / SQ) /* samples in half a width */
#define STROKE_PAD 4   /* rows the stroke may reach above or below the chart */
#define DOT_RADIUS (SQ * 33 / 10)
/* The widest chart: one point a column, as the highs-and-lows line has. */
#define LINE_W (BOARD_LCD_V_RES - 2 * MARGIN)

/* @p n heights spread evenly across a chart @p w columns wide, in 1/32 px
 * down from the chart's top. Whole numbers from here on: this chip has no
 * floating-point unit, and all of this runs twice a frame while a page is
 * changing. */
typedef struct {
    const int16_t *y;
    int n, w;
} heights_t;

/* The height at pixel column @p col. */
static int height_at(const heights_t *hs, int col)
{
    int at = col * (hs->n - 1);
    int i = at / (hs->w - 1), rem = at % (hs->w - 1);
    return i + 1 < hs->n ? hs->y[i] + (hs->y[i + 1] - hs->y[i]) * rem / (hs->w - 1) : hs->y[i];
}

/* Walks a line left to right, giving its height at each sample. */
typedef struct {
    const heights_t *hs;
    int span;            /* from the first point to the last, 1/32 px */
    int seg, xa, xb;     /* the segment under the walker, and its ends */
    int half;            /* how far the stroke reaches above and below it */
} stroke_walk_t;

/* Take segment @p seg. The stroke's reach is its half width stretched by
 * the slope, so a steep run is as thick as a flat one. */
static void stroke_segment(stroke_walk_t *s, int seg)
{
    const heights_t *hs = s->hs;
    s->seg = seg;
    s->xa = seg * s->span / (hs->n - 1);
    s->xb = (seg + 1) * s->span / (hs->n - 1);
    int adx = s->xb - s->xa;
    int ady = hs->y[seg + 1] - hs->y[seg];
    ady = ady < 0 ? -ady : ady;
    /* The segment's length without a square root: within 7%. */
    int len = adx > ady ? adx + ady * 3 / 8 : ady + adx * 3 / 8;
    s->half = adx > 0 ? STROKE_HALF * len / adx : STROKE_HALF;
}

/* The line's height @p t along it (0 = the first point), and its reach
 * there in @p half: -1 beyond the ends. @p t must not decrease from one
 * call to the next. */
static int stroke_sample(stroke_walk_t *s, int t, int *half)
{
    const heights_t *hs = s->hs;
    if (t <= 0 || t >= s->span) {
        *half = (t < -STROKE_HALF || t > s->span + STROKE_HALF) ? -1 : STROKE_HALF;
        return t <= 0 ? hs->y[0] : hs->y[hs->n - 1];
    }
    while (t >= s->xb && s->seg < hs->n - 2) {
        stroke_segment(s, s->seg + 1);
    }
    *half = s->half;
    int adx = s->xb - s->xa;
    return adx > 0 ? hs->y[s->seg] + (hs->y[s->seg + 1] - hs->y[s->seg]) * (t - s->xa) / adx
                   : hs->y[s->seg];
}

static int isqrt(int v)
{
    int r = 0;
    for (int bit = 1 << 14; bit; bit >>= 1) {
        if ((r + bit) * (r + bit) <= v) {
            r += bit;
        }
    }
    return r;
}

/* A filled disc with a blended rim; centre and radius in 1/32 px. */
static void draw_disc_aa(uint16_t *fb, int cxq, int cyq, int rq, uint16_t color)
{
    for (int py = (cyq - rq) / SQ - 1; py <= (cyq + rq) / SQ + 1; py++) {
        for (int px = (cxq - rq) / SQ - 1; px <= (cxq + rq) / SQ + 1; px++) {
            int dx = px * SQ + SQ / 2 - cxq, dy = py * SQ + SQ / 2 - cyq;
            int cover = rq + SQ / 2 - isqrt(dx * dx + dy * dy);
            if (cover > 0) {
                board_gfx_blend_pixel(fb, px, py, color,
                                      (uint8_t)(cover >= SQ ? 255 : cover * 255 / SQ));
            }
        }
    }
}

/* The line through @p hs as a smooth 2 px stroke ending in the live dot,
 * with the chart's top-left at (x, y).
 *
 * The line never doubles back, so it is drawn a pixel column at a time: at
 * a couple of places across the column the stroke covers a run of rows, and
 * each pixel takes the line's colour by how much of it those runs cover. At
 * a peak or a trough the run is cut off a half width past the turn, or the
 * stretch for the slope would draw a spike there. */
static void draw_stroke_aa(uint16_t *fb, int x, int y, const heights_t *hs, uint16_t color)
{
    stroke_walk_t s = {.hs = hs, .span = (hs->w - 1) * SQ};
    stroke_segment(&s, 0);

    /* Only the columns inside the clip window: most of a page that is
     * sliding in is not on screen yet. One column either side of the chart
     * as well, where the stroke's ends are capped. */
    int cx0, cy0, cx1, cy1;
    board_gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
    int col_from = cx0 - x > -1 ? cx0 - x : -1;
    int col_to = cx1 - 1 - x < hs->w ? cx1 - 1 - x : hs->w;

    /* ring[] holds the sample being drawn with the STROKE_WIN either side
     * of it, which is where a nearby turn shows up. */
    enum { RING = 2 * STROKE_WIN + 1, STEP = SQ / STROKE_SUB };
    int ring_y[RING], ring_half[RING];
    /* Sample i sits at this distance along the line. */
#define SAMPLE_T(i) ((i) * STEP + STEP / 2 - SQ - SQ / 2)
    int first = (col_from + 1) * STROKE_SUB;
    for (int i = 0; i < RING - 1; i++) {
        ring_y[i] = stroke_sample(&s, SAMPLE_T(first + i - STROKE_WIN), &ring_half[i]);
    }
    int newest = RING - 1; /* where the next sample goes; the middle is STROKE_WIN on */

    rgb_t ink = unpack(color);
    int stride = board_display_width();
    uint16_t acc[SPARK_H + 2 * STROKE_PAD] = {0};
    const int rows = (int)(sizeof(acc) / sizeof(acc[0]));
    for (int col = col_from; col <= col_to; col++) {
        int r_lo = rows, r_hi = -1;
        for (int sub = 0; sub < STROKE_SUB; sub++) {
            int i = (col + 1) * STROKE_SUB + sub;
            ring_y[newest] = stroke_sample(&s, SAMPLE_T(i + STROKE_WIN), &ring_half[newest]);
            newest = (newest + 1) % RING;
            int mid = (newest + STROKE_WIN) % RING;

            int yc = ring_y[mid], half = ring_half[mid];
            if (half < 0) {
                continue;
            }
            int near_lo = yc, near_hi = yc;
            for (int j = 0; j < RING; j++) {
                near_lo = ring_y[j] < near_lo ? ring_y[j] : near_lo;
                near_hi = ring_y[j] > near_hi ? ring_y[j] : near_hi;
            }
            int top = yc - half, bot = yc + half;
            top = top < near_lo - STROKE_HALF ? near_lo - STROKE_HALF : top;
            bot = bot > near_hi + STROKE_HALF ? near_hi + STROKE_HALF : bot;
            top += STROKE_PAD * SQ; /* into acc[]'s rows, and clear of zero */
            bot += STROKE_PAD * SQ;
            int r0 = top / SQ, r1 = (bot - 1) / SQ;
            r0 = r0 < 0 ? 0 : r0;
            r1 = r1 >= rows ? rows - 1 : r1;
            for (int r = r0; r <= r1; r++) {
                int from = top > r * SQ ? top : r * SQ;
                int to = bot < (r + 1) * SQ ? bot : (r + 1) * SQ;
                acc[r] = (uint16_t)(acc[r] + to - from);
            }
            r_lo = r0 < r_lo ? r0 : r_lo;
            r_hi = r1 > r_hi ? r1 : r_hi;
        }
        uint16_t *px = fb + x + col;
        for (int r = r_lo; r <= r_hi; r++) {
            int a = acc[r] * 256 / (STROKE_SUB * SQ);
            int py = y + r - STROKE_PAD;
            acc[r] = 0;
            if (py >= cy0 && py < cy1) {
                blend_into(px + py * stride, ink, a > 256 ? 256 : a);
            }
        }
    }
#undef SAMPLE_T

    draw_disc_aa(fb, x * SQ + SQ / 2 + s.span, y * SQ + hs->y[hs->n - 1], DOT_RADIUS, color);
}

/* The line through @p v. Under it either a gradient fill or, given
 * @p band_lo and @p band_hi (one per point of v), a band between them. */
static void draw_sparkline(uint16_t *fb, int x, int y, int w, int h, const float *v, int n,
                           const float *band_lo, const float *band_hi, uint16_t line_color,
                           uint16_t fill_color, uint8_t fill_alpha)
{
    if (n < 2 || n > LINE_W) {
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

    /* Every value to a height once, here; the rest is whole numbers. The
     * line's centre runs from row 1 for the highest value to row h for the
     * lowest, so its 2 px sit on the chart's last two rows there. */
    static int16_t line_y[LINE_W], band_top[APP_CHART_MAX], band_bot[APP_CHART_MAX];
    float scale = (float)(h - 1) / range * SQ;
    for (int i = 0; i < n; i++) {
        line_y[i] = (int16_t)(h * SQ - (int)((v[i] - lo) * scale));
    }
    const heights_t line = {line_y, n, w};
    heights_t top = {band_top, n, w}, bot = {band_bot, n, w};
    if (band_lo && n <= APP_CHART_MAX) {
        for (int i = 0; i < n; i++) {
            band_top[i] = (int16_t)((h - 1) * SQ - (int)((band_hi[i] - lo) * scale));
            band_bot[i] = (int16_t)(h * SQ - (int)((band_lo[i] - lo) * scale));
        }
    } else {
        band_lo = NULL;
    }

    /* Columns and rows outside the clip window are skipped here rather than
     * pixel by pixel: most of a page that is sliding in is not on screen. */
    int cx0, cy0, cx1, cy1;
    board_gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
    int y_end = y + h < cy1 ? y + h : cy1;

    /* While the page is moving, fill every other column: motion hides the
     * decimation completely, and the saved blends roughly double the
     * transition frame rate's headroom. Not in the last few pixels of a
     * glide, where the page crawls and the switch back to every column
     * would show as a flicker on landing. */
    enum { CRAWL_PX = 8 };
    int col_step = (abs(s_xoff) > CRAWL_PX || abs(s_yoff) > CRAWL_PX) ? 2 : 1;
    rgb_t ink = unpack(fill_color);
    int stride = board_display_width();
    for (int col = 0; col < w; col += col_step) {
        if (x + col < cx0 || x + col >= cx1) {
            continue;
        }
        uint16_t *px = fb + x + col;
        if (band_lo) {
            /* The band's edges fall between rows: the rows they cut are
             * blended by how much of each the band covers. */
            int from = height_at(&top, col), to = height_at(&bot, col);
            int a = BAND_ALPHA * s_alpha / 255; /* of 255, near enough of 256 */
            for (int r = from / SQ; r <= (to - 1) / SQ; r++) {
                int r_from = from > r * SQ ? from : r * SQ;
                int r_to = to < (r + 1) * SQ ? to : (r + 1) * SQ;
                if (y + r >= cy0 && y + r < cy1) {
                    blend_into(px + (y + r) * stride, ink, a * (r_to - r_from) / SQ);
                }
            }
            continue;
        }
        /* Gradient fill, from the row the line's centre is in: the line's
         * colour dissolving toward the bottom edge, scaled by page alpha so
         * fades take the fill down with them. The opacity falls by the same
         * amount each row: 8.8 fixed point. */
        int cy = y + height_at(&line, col) / SQ;
        int depth = (y + h) - cy + 1;
        int fall = ((fill_alpha * s_alpha / 255) << 8) / depth;
        for (int py = cy < cy0 ? cy0 : cy; py < y_end; py++) {
            blend_into(px + py * stride, ink, (fall * ((y + h) - py)) >> 8);
        }
    }

    draw_stroke_aa(fb, x, y, &line, line_color);
}

/* A smooth line through the highs and lows of each swing, one value per
 * column of a chart @p w wide.
 *
 * Working it out is the dearest thing on a page -- a third of a frame --
 * and it only changes when a price does, so the last two are kept: two,
 * because a page change draws two coins every frame. */

static const float *extremes_line(const ui_coin_t *c, int w)
{
    static struct {
        uint32_t key;
        int n;
        float columns[LINE_W];
    } kept[2];
    static int oldest;
    static chart_pt_t pts[2 * APP_CHART_MAX + 1], swings[2 * APP_CHART_MAX + 1];

    /* FNV-1a over the candles: any tick changes the newest close. */
    uint32_t key = 2166136261u;
    const float *series[] = {c->closes, c->highs, c->lows};
    for (int k = 0; k < 3; k++) {
        const uint8_t *p = (const uint8_t *)series[k];
        for (size_t i = 0; i < sizeof(float) * (size_t)c->n_closes; i++) {
            key = (key ^ p[i]) * 16777619u;
        }
    }
    for (int k = 0; k < 2; k++) {
        if (kept[k].n == c->n_closes && kept[k].key == key) {
            return kept[k].columns;
        }
    }
    int slot = oldest;
    oldest ^= 1;
    int n = chart_extremes(c->closes, c->highs, c->lows, c->n_closes, pts);
    n = chart_swings(pts, n, CHART_MIN_SWING, swings);
    chart_columns(swings, n, w > LINE_W ? LINE_W : w, kept[slot].columns);
    kept[slot].key = key;
    kept[slot].n = c->n_closes;
    return kept[slot].columns;
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

/* Rows of the page, top to bottom: where each starts and the last ends. */
static const int k_row_y[UI_PAGE_ROWS + 1] = {0, 60, 98, BOARD_LCD_H_RES};

/* Make @p shift the draw state for row @p row. False if nothing of the row
 * can show, so its drawing is skipped altogether. */
static bool row_begin(const ui_shift_t *shift, int row)
{
    int cx0, cy0, cx1, cy1;
    board_gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
    if (shift->alpha == 0 || shift->dx >= cx1 || shift->dx + board_display_width() <= cx0 ||
        k_row_y[row] + shift->dy >= cy1 || k_row_y[row + 1] + shift->dy <= cy0) {
        return false;
    }
    s_alpha = shift->alpha;
    s_xoff = shift->dx;
    s_yoff = shift->dy;
    return true;
}

/* Title line: symbol in white, price in the day's colour, same size. */
static void draw_title(uint16_t *fb, const ui_coin_t *c)
{
    int y = TITLE_Y + s_yoff;
    int pen = board_font_text(fb, MARGIN + s_xoff, y, c->label, &font_large, page_col(COL_TEXT));

    if (c->last <= 0.0f) {
        board_font_text(fb, pen + 14, y, "--", &font_large, page_col(COL_MUTED));
        return;
    }
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
    board_font_text(fb, pen + 14, y, price, &font_large, page_col(price_col));
}

/* One change group: arrow, percentage and what it is measured over, with
 * its left edge at @p gx. */
static void draw_change(uint16_t *fb, const ui_coin_t *c, int gx, float base, const char *tag)
{
    int y = CHANGE_Y + s_yoff;
    bool up = c->last >= base;
    uint16_t col = page_col(c->stale ? COL_MUTED : (up ? COL_UP : COL_DOWN));
    char pct[16];
    snprintf(pct, sizeof(pct), "%.2f%%", (double)fabsf((c->last - base) / base * 100.0f));

    draw_triangle_aa(fb, gx, y + (up ? 7 : 8), 15, up, col);
    int tx = board_font_text(fb, gx + 24, y, pct, &font_medium, col);
    board_font_text(fb, tx + 10, y + 7, tag, &font_small, page_col(COL_MUTED));
}

static void draw_changes(uint16_t *fb, const ui_coin_t *c)
{
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
        draw_change(fb, c, MARGIN + s_xoff, base, tag);
    }

    /* Range change, right-aligned: measured against the active chart
     * range's oldest close, labelled to match -- 1D, 7D or 30D. Long
     * pressing the button cycles the range, and this group follows. */
    if (c->n_closes >= 2 && c->closes[0] > 0.0f) {
        static const app_range_t k_range_cfg[APP_NUM_RANGES] = APP_RANGES;
        const char *range_tag = k_range_cfg[c->range].tag;

        char pct[16];
        snprintf(pct, sizeof(pct), "%.2f%%",
                 (double)fabsf((c->last - c->closes[0]) / c->closes[0] * 100.0f));
        int width = 24 + board_font_text_width(pct, &font_medium) + 10 +
                    board_font_text_width(range_tag, &font_small);
        draw_change(fb, c, s_xoff + board_display_width() - MARGIN - width, c->closes[0],
                    range_tag);
    }
}

static void draw_chart(uint16_t *fb, const ui_coin_t *c)
{
    int x = MARGIN + s_xoff, y = SPARK_Y + s_yoff;
    int w = board_display_width() - 2 * MARGIN;
    if (c->n_closes < 2) {
        board_gfx_hline(fb, x, y + SPARK_H / 2, w, page_col(COL_GRID));
        return;
    }
    /* Coloured by its own span: first stored close vs the newest. */
    bool up = c->closes[c->n_closes - 1] >= c->closes[0];
    uint16_t line = c->stale ? COL_MUTED : (up ? COL_UP : COL_DOWN);
    uint16_t fill = up ? mix_col(line, COL_WHITE, FILL_LIGHTEN) : COL_DOWN_FILL;
    if (c->stale) {
        fill = mix_col(COL_MUTED, COL_WHITE, FILL_LIGHTEN);
    }
    const float *v = c->closes;
    int n = c->n_closes;
    if (c->chart == UI_CHART_HIGHS_LOWS) {
        v = extremes_line(c, w);
        n = w;
    }
    bool band = c->chart == UI_CHART_BAND;
    draw_sparkline(fb, x, y, w, SPARK_H, v, n, band ? c->lows : NULL, band ? c->highs : NULL,
                   page_col(line), page_col(fill), up ? FILL_ALPHA_TOP : FILL_ALPHA_TOP_DOWN);
}

void ui_draw_page_rows(uint16_t *fb, const ui_coin_t *c, const ui_shift_t rows[UI_PAGE_ROWS])
{
    if (row_begin(&rows[0], 0)) {
        draw_title(fb, c);
    }
    if (c->last > 0.0f && row_begin(&rows[1], 1)) {
        draw_changes(fb, c);
    }
    if (row_begin(&rows[2], 2)) {
        draw_chart(fb, c);
    }
    s_alpha = 255;
    s_xoff = 0;
    s_yoff = 0;
}

void ui_draw_page(uint16_t *fb, const ui_coin_t *c, int x_off, uint8_t alpha)
{
    ui_shift_t rows[UI_PAGE_ROWS];
    for (int i = 0; i < UI_PAGE_ROWS; i++) {
        rows[i] = (ui_shift_t){.dx = (int16_t)x_off, .alpha = alpha};
    }
    ui_draw_page_rows(fb, c, rows);
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
