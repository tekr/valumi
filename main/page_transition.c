#include "page_transition.h"

#include <math.h>

#include "board_display.h"
#include "board_gfx.h"
#include "settings.h"

/* The display has one frame buffer and there is no room for a second, so a
 * page cannot be drawn aside and then moved: every frame here is both pages
 * drawn straight into place, shifted, dimmed or clipped as they go. */

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float ease_in_out(float t)
{
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t); /* smoothstep */
}

/* How far a page has travelled: a quadratic-warped start into a cubic
 * settle. A quartic tail goes sub-pixel with ~15% of the duration left, so
 * the page visibly parks early; this keeps the final ~20 px crawling at 1-5
 * px per frame over the last third, which the eye can see decelerate. */
static float glide(float p)
{
    p = clamp01(p);
    float pw = p * p * (2.0f - p);
    float uw = 1.0f - pw;
    return 1.0f - uw * uw * uw;
}

/* The fade of the outgoing and incoming page at @p p. The crossfade is done
 * by 75% so the landing happens on a solid page.
 * CROSSFADE: the old page fades as the new one appears, so even at 100% the
 * screen is never empty. DIP: out over the first half, in over the second,
 * so at 100% there is a moment of bare background. */
static void fades(const page_transition_t *t, float p, uint8_t *out, uint8_t *in)
{
    float mix = ease_in_out(p / 0.75f);
    float depth = (float)t->fade / SET_FADE_MAX;
    float out_dim = mix, in_dim = 1.0f - mix;
    if (t->style == SET_STYLE_DIP) {
        out_dim = fminf(mix * 2.0f, 1.0f);
        in_dim = fminf((1.0f - mix) * 2.0f, 1.0f);
    }
    *out = (uint8_t)((1.0f - depth * out_dim) * 255.0f);
    *in = (uint8_t)((1.0f - depth * in_dim) * 255.0f);
}

/* A shift of @p d along the axis the pages change on. */
static ui_shift_t along(const page_transition_t *t, int d, uint8_t alpha)
{
    return t->vertical ? (ui_shift_t){.dy = (int16_t)d, .alpha = alpha}
                       : (ui_shift_t){.dx = (int16_t)d, .alpha = alpha};
}

static void draw_whole(uint16_t *fb, const ui_coin_t *c, ui_shift_t shift)
{
    ui_shift_t rows[UI_PAGE_ROWS];
    for (int i = 0; i < UI_PAGE_ROWS; i++) {
        rows[i] = shift;
    }
    ui_draw_page_rows(fb, c, rows);
}

/* Clip to the stretch from @p from to @p to along the axis. */
static void clip_along(const page_transition_t *t, int from, int to)
{
    if (t->vertical) {
        board_gfx_set_clip(0, from, board_display_width(), to - from);
    } else {
        board_gfx_set_clip(from, 0, to - from, board_display_height());
    }
}

/* Both pages travel together, as one strip. */
static void slide(uint16_t *fb, const page_transition_t *t, const ui_coin_t *from,
                  const ui_coin_t *to, float p, int dir, int span)
{
    int d = (int)(glide(p) * (float)span);
    uint8_t a_out, a_in;
    fades(t, p, &a_out, &a_in);
    /* Mirroring the offsets is the whole of "the other way". */
    draw_whole(fb, from, along(t, -d * dir, a_out));
    draw_whole(fb, to, along(t, (span - d) * dir, a_in));
}

/* A slide in which the three rows of the page set off one after another.
 *
 * Sideways, each row is a strip of its own: row r of the old page and row r
 * of the new travel together, later the further down the page they are.
 * Up and down the rows share one strip, so they go as a train -- the row at
 * the front first, each behind it a little later, the new page's rows after
 * the old one's -- or a row would run into the one ahead of it. */
static void cascade(uint16_t *fb, const page_transition_t *t, const ui_coin_t *from,
                    const ui_coin_t *to, float p, int dir, int span)
{
    ui_shift_t out[UI_PAGE_ROWS], in[UI_PAGE_ROWS];
    for (int r = 0; r < UI_PAGE_ROWS; r++) {
        float q_out, q_in;
        if (t->vertical) {
            const float lag = 0.09f, run = 1.0f - lag * (2 * UI_PAGE_ROWS - 2);
            int place = dir > 0 ? r : UI_PAGE_ROWS - 1 - r; /* 0 = the front of the page */
            q_out = (p - lag * (float)place) / run;
            q_in = (p - lag * (float)(place + UI_PAGE_ROWS - 1)) / run;
        } else {
            const float lag = 0.14f, run = 1.0f - lag * (UI_PAGE_ROWS - 1);
            q_out = q_in = (p - lag * (float)r) / run;
        }
        uint8_t a_out, a_in, unused;
        fades(t, clamp01(q_out), &a_out, &unused);
        fades(t, clamp01(q_in), &unused, &a_in);
        out[r] = along(t, -(int)(glide(q_out) * (float)span) * dir, a_out);
        in[r] = along(t, (span - (int)(glide(q_in) * (float)span)) * dir, a_in);
    }
    ui_draw_page_rows(fb, from, out);
    ui_draw_page_rows(fb, to, in);
}

/* The stretch of the axis the new page has taken so far, when it has come
 * @p d of the way from the side @p dir says. */
static void taken(int d, int dir, int span, int *from, int *to)
{
    *from = dir > 0 ? span - d : 0;
    *to = dir > 0 ? span : d;
}

/* The new page travels in over the old one, which drifts a quarter as far
 * the same way and dims under the shadow of the new page's leading edge. */
static void cover(uint16_t *fb, const page_transition_t *t, const ui_coin_t *from,
                  const ui_coin_t *to, float p, int dir, int span)
{
    enum { SHADOW = 18 };
    int d = (int)(glide(p) * (float)span);
    int new_from, new_to;
    taken(d, dir, span, &new_from, &new_to);
    int old_from = dir > 0 ? 0 : new_to, old_to = dir > 0 ? new_from : span;

    float dim = 0.8f * (float)t->fade / SET_FADE_MAX * ease_in_out(p / 0.9f);
    clip_along(t, old_from, old_to);
    draw_whole(fb, from, along(t, -(d / 4) * dir, (uint8_t)((1.0f - dim) * 255.0f)));
    /* The shadow: darkest against the edge, gone SHADOW px from it. */
    for (int s = 0; s < SHADOW; s++) {
        int at = dir > 0 ? new_from - 1 - s : new_to + s;
        int rest = SHADOW - s;
        uint8_t keep = (uint8_t)(255 - 180 * rest * rest / (SHADOW * SHADOW));
        if (t->vertical) {
            board_gfx_dim_rect(fb, 0, at, board_display_width(), 1, keep);
        } else {
            board_gfx_dim_rect(fb, at, 0, 1, board_display_height(), keep);
        }
    }

    clip_along(t, new_from, new_to);
    draw_whole(fb, to, along(t, (span - d) * dir, 255));
    board_gfx_reset_clip();
}

/* Neither page moves: an edge sweeps across, the new page behind it. */
static void wipe(uint16_t *fb, const page_transition_t *t, const ui_coin_t *from,
                 const ui_coin_t *to, float p, int dir, int span)
{
    enum { EDGE = 2 };
    /* The edge itself has to leave the screen too. */
    int d = (int)(ease_in_out(p) * (float)(span + EDGE));
    int new_from, new_to;
    taken(d, dir, span, &new_from, &new_to);

    clip_along(t, dir > 0 ? 0 : new_to, dir > 0 ? new_from : span);
    draw_whole(fb, from, along(t, 0, 255));
    clip_along(t, new_from, new_to);
    draw_whole(fb, to, along(t, 0, 255));
    board_gfx_reset_clip();

    /* A thin line of light on the edge, brightest mid-way. */
    float glow = 0.55f * sinf(3.14159265f * clamp01(p));
    uint16_t line = board_rgb565((uint8_t)(235.0f * glow), (uint8_t)(238.0f * glow),
                                 (uint8_t)(245.0f * glow));
    int at = dir > 0 ? new_from - EDGE : new_to;
    if (t->vertical) {
        board_gfx_fill_rect(fb, 0, at, board_display_width(), EDGE, line);
    } else {
        board_gfx_fill_rect(fb, at, 0, EDGE, board_display_height(), line);
    }
}

void page_transition_render(uint16_t *fb, const page_transition_t *t, const ui_coin_t *from,
                            const ui_coin_t *to, float p, int dir)
{
    int span = t->vertical ? board_display_height() : board_display_width();
    dir = dir < 0 ? -1 : 1;
    ui_begin_frame(fb);
    switch (t->move) {
    case SET_MOVE_COVER:
        cover(fb, t, from, to, p, dir, span);
        break;
    case SET_MOVE_CASCADE:
        cascade(fb, t, from, to, p, dir, span);
        break;
    case SET_MOVE_WIPE:
        wipe(fb, t, from, to, p, dir, span);
        break;
    default:
        slide(fb, t, from, to, p, dir, span);
        break;
    }
}
