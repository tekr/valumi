#include "board_gfx.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* Logical dimensions depend on the orientation chosen at display init, so
 * clipping reads them at runtime rather than baking in the portrait size. */
#define W board_display_width()
#define H board_display_height()

/* Classic 5x7 cell font, ASCII 0x20-0x5F. Each glyph is five column bytes,
 * bit 0 = top row. A sixth blank column is added at draw time for spacing. */
static const uint8_t s_font5x7[96][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x07, 0x00, 0x07, 0x00}, /* " */
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, /* # */
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, /* $ */
    {0x23, 0x13, 0x08, 0x64, 0x62}, /* % */
    {0x36, 0x49, 0x55, 0x22, 0x50}, /* & */
    {0x00, 0x05, 0x03, 0x00, 0x00}, /* ' */
    {0x00, 0x1C, 0x22, 0x41, 0x00}, /* ( */
    {0x00, 0x41, 0x22, 0x1C, 0x00}, /* ) */
    {0x14, 0x08, 0x3E, 0x08, 0x14}, /* * */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x00, 0x50, 0x30, 0x00, 0x00}, /* , */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x56, 0x36, 0x00, 0x00}, /* ; */
    {0x08, 0x14, 0x22, 0x41, 0x00}, /* < */
    {0x14, 0x14, 0x14, 0x14, 0x14}, /* = */
    {0x00, 0x41, 0x22, 0x14, 0x08}, /* > */
    {0x02, 0x01, 0x51, 0x09, 0x06}, /* ? */
    {0x32, 0x49, 0x79, 0x41, 0x3E}, /* @ */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x7F, 0x41, 0x41, 0x00}, /* [ */
    {0x02, 0x04, 0x08, 0x10, 0x20}, /* backslash */
    {0x00, 0x41, 0x41, 0x7F, 0x00}, /* ] */
    {0x04, 0x02, 0x01, 0x02, 0x04}, /* ^ */
    {0x40, 0x40, 0x40, 0x40, 0x40}, /* _ */
};

#define FONT_LAST_CHAR 0x5F

/* The clip window; x1/y1 of 0 mean "the whole screen" so that nothing has to
 * initialise it before the display's size is known. */
static int s_cx0, s_cy0, s_cx1, s_cy1;

void board_gfx_set_clip(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    s_cx0 = x < 0 ? 0 : x;
    s_cy0 = y < 0 ? 0 : y;
    s_cx1 = x1 > W ? W : x1;
    s_cy1 = y1 > H ? H : y1;
    if (s_cx1 <= s_cx0 || s_cy1 <= s_cy0) {
        /* Empty: a window nothing can fall inside, not "the whole screen". */
        s_cx0 = s_cy0 = 0;
        s_cx1 = s_cy1 = -1;
    }
}

void board_gfx_reset_clip(void)
{
    s_cx0 = s_cy0 = s_cx1 = s_cy1 = 0;
}

void board_gfx_get_clip(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = s_cx0;
    *y0 = s_cy0;
    *x1 = s_cx1 == 0 ? W : (s_cx1 < 0 ? 0 : s_cx1);
    *y1 = s_cy1 == 0 ? H : (s_cy1 < 0 ? 0 : s_cy1);
}

static inline bool clipped(int x, int y)
{
    if ((unsigned)x >= W || (unsigned)y >= H) {
        return true;
    }
    return s_cx1 != 0 && (x < s_cx0 || x >= s_cx1 || y < s_cy0 || y >= s_cy1);
}

void board_gfx_clear(uint16_t *fb, uint16_t color)
{
    /* memset only helps when both bytes match; otherwise fall back to a loop
     * the compiler can unroll. */
    if ((color & 0xFF) == (color >> 8)) {
        memset(fb, color & 0xFF, BOARD_FB_BYTES);
        return;
    }
    for (int i = 0; i < BOARD_FB_PIXELS; i++) {
        fb[i] = color;
    }
}

void board_gfx_pixel(uint16_t *fb, int x, int y, uint16_t color)
{
    if (clipped(x, y)) {
        return;
    }
    fb[y * W + x] = color;
}

void board_gfx_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    /* Clip rather than reject; partially visible shapes are the common case
     * for anything moving. */
    int cx0, cy0, cx1, cy1;
    board_gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
    int x0 = x < cx0 ? cx0 : x;
    int y0 = y < cy0 ? cy0 : y;
    int x1 = x + w > cx1 ? cx1 : x + w;
    int y1 = y + h > cy1 ? cy1 : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (int row = y0; row < y1; row++) {
        uint16_t *p = fb + row * W + x0;
        for (int col = x0; col < x1; col++) {
            *p++ = color;
        }
    }
}

void board_gfx_dim_rect(uint16_t *fb, int x, int y, int w, int h, uint8_t keep)
{
    int cx0, cy0, cx1, cy1;
    board_gfx_get_clip(&cx0, &cy0, &cx1, &cy1);
    int x0 = x < cx0 ? cx0 : x;
    int y0 = y < cy0 ? cy0 : y;
    int x1 = x + w > cx1 ? cx1 : x + w;
    int y1 = y + h > cy1 ? cy1 : y + h;
    int k = keep + (keep >> 7); /* of 256, so the scaling is a shift */
    int stride = W;

    for (int row = y0; row < y1; row++) {
        uint16_t *p = fb + row * stride + x0;
        for (int col = x0; col < x1; col++, p++) {
            /* Stored big-endian for the panel: see board_rgb565(). */
            uint16_t c = (uint16_t)((*p >> 8) | (*p << 8));
            if (c == 0) {
                continue;
            }
            uint16_t out = (uint16_t)(((((c >> 11) & 0x1F) * k >> 8) << 11) |
                                      ((((c >> 5) & 0x3F) * k >> 8) << 5) | ((c & 0x1F) * k >> 8));
            *p = (uint16_t)((out >> 8) | (out << 8));
        }
    }
}

void board_gfx_hline(uint16_t *fb, int x, int y, int w, uint16_t color)
{
    board_gfx_fill_rect(fb, x, y, w, 1, color);
}

void board_gfx_vline(uint16_t *fb, int x, int y, int h, uint16_t color)
{
    board_gfx_fill_rect(fb, x, y, 1, h, color);
}

void board_gfx_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    board_gfx_hline(fb, x, y, w, color);
    board_gfx_hline(fb, x, y + h - 1, w, color);
    board_gfx_vline(fb, x, y, h, color);
    board_gfx_vline(fb, x + w - 1, y, h, color);
}

void board_gfx_fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t color)
{
    if (r <= 0) {
        return;
    }
    /* Span-per-scanline; cheaper than a per-pixel distance test and gives the
     * same result for a filled disc. */
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        int dx = 0;
        while (dx * dx + dy * dy <= r * r) {
            dx++;
        }
        dx--;
        board_gfx_hline(fb, cx - dx, y, 2 * dx + 1, color);
    }
}

void board_gfx_blend_pixel(uint16_t *fb, int x, int y, uint16_t color, uint8_t alpha)
{
    if (clipped(x, y)) {
        return;
    }
    if (alpha == 0) {
        return;
    }
    uint16_t *p = fb + y * W + x;
    if (alpha >= 255) {
        *p = color;
        return;
    }

    /* Pixels are stored byte-swapped (panel wire order): unswap, lerp each
     * channel, reswap. Opacity out of 256 rather than 255, so the lerp is a
     * shift: this is the inner loop of everything that fades. */
    uint16_t bg = (uint16_t)((*p >> 8) | (*p << 8));
    uint16_t fg = (uint16_t)((color >> 8) | (color << 8));
    int a = alpha + (alpha >> 7);

    int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
    r += ((int)((fg >> 11) & 0x1F) - r) * a >> 8;
    g += ((int)((fg >> 5) & 0x3F) - g) * a >> 8;
    b += ((int)(fg & 0x1F) - b) * a >> 8;

    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    *p = (uint16_t)((out >> 8) | (out << 8));
}

void board_gfx_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t color)
{
    /* Bresenham; per-pixel clipping via board_gfx_pixel is fine at the segment
     * lengths a 320-px-wide chart produces. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        board_gfx_pixel(fb, x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int board_gfx_text_width(const char *str, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    return (int)strlen(str) * BOARD_GFX_GLYPH_W * scale;
}

int board_gfx_text(uint16_t *fb, int x, int y, const char *str, uint16_t color, int scale)
{
    if (scale < 1) {
        scale = 1;
    }

    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') {
            c -= 32; /* the font has no lowercase */
        }
        if (c < 0x20 || c > FONT_LAST_CHAR) {
            c = '?';
        }
        const uint8_t *glyph = s_font5x7[c - 0x20];

        for (int col = 0; col < 5; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1 << row)) {
                    if (scale == 1) {
                        board_gfx_pixel(fb, x + col, y + row, color);
                    } else {
                        board_gfx_fill_rect(fb, x + col * scale, y + row * scale, scale, scale,
                                            color);
                    }
                }
            }
        }
        x += BOARD_GFX_GLYPH_W * scale;
    }
    return x;
}

void board_gfx_rotate_blit(uint16_t *dst, const uint16_t *src, int src_w, int src_h,
                           float radians, uint16_t bg)
{
    const int w = W, h = H;
    const float cx = (float)w * 0.5f, cy = (float)h * 0.5f;

    float c = cosf(radians), s = sinf(radians);
    float ac = fabsf(c), as = fabsf(s);

    /* Uniform scale at which the rotated w*h rectangle still fits inside w*h.
     * The rotated bounding box is (w*|cos| + h*|sin|) by (w*|sin| + h*|cos|),
     * so each axis gives a limit and the tighter one wins. Guarded against a
     * zero denominator, which cannot happen for a non-degenerate screen but
     * would be a silent divide by zero if it did. */
    float bw = (float)w * ac + (float)h * as;
    float bh = (float)w * as + (float)h * ac;
    float k = 1.0f;
    if (bw > 0.0f && bh > 0.0f) {
        float kx = (float)w / bw, ky = (float)h / bh;
        k = kx < ky ? kx : ky;
    }

    /* Walk the DESTINATION and sample the source, so every output pixel is
     * written exactly once -- mapping forward would leave holes wherever the
     * scale magnifies. The inverse map is
     *     src = centre + (1/k) * R(-radians) * (dst - centre)
     * then scaled into the source's own resolution. It is affine, so the inner
     * loop is two adds. 16.16 fixed point: the screen is under 512 px, leaving
     * ample headroom in the integer half. */
    float inv = 1.0f / k;
    float rx = (float)src_w / (float)w;
    float ry = (float)src_h / (float)h;

    const int32_t dsx_dx = (int32_t)(c * inv * rx * 65536.0f);
    const int32_t dsy_dx = (int32_t)(-s * inv * ry * 65536.0f);

    for (int y = 0; y < h; y++) {
        float fy = (float)y + 0.5f - cy;
        float fx = 0.5f - cx;
        int32_t sx = (int32_t)(((cx + inv * (c * fx + s * fy)) * rx) * 65536.0f);
        int32_t sy = (int32_t)(((cy + inv * (-s * fx + c * fy)) * ry) * 65536.0f);

        uint16_t *out = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int ix = sx >> 16;
            int iy = sy >> 16;
            /* Unsigned compare catches negatives in one branch. */
            *out++ = ((unsigned)ix < (unsigned)src_w && (unsigned)iy < (unsigned)src_h)
                         ? src[(size_t)iy * src_w + ix]
                         : bg;
            sx += dsx_dx;
            sy += dsy_dx;
        }
    }
}

void board_gfx_shrink_half(uint16_t *dst, const uint16_t *src)
{
    const int w = W, h = H;
    const int dw = w / 2, dh = h / 2;

    for (int y = 0; y < dh; y++) {
        const uint16_t *r0 = src + (size_t)(y * 2) * w;
        const uint16_t *r1 = r0 + w;
        uint16_t *out = dst + (size_t)y * dw;

        for (int x = 0; x < dw; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int i = 0; i < 2; i++) {
                /* Stored big-endian for the panel, so swap before unpacking
                 * and swap back after -- see board_rgb565(). */
                uint16_t a = r0[x * 2 + i];
                uint16_t d = r1[x * 2 + i];
                a = (uint16_t)((a >> 8) | (a << 8));
                d = (uint16_t)((d >> 8) | (d << 8));
                r += ((a >> 11) & 0x1F) + ((d >> 11) & 0x1F);
                g += ((a >> 5) & 0x3F) + ((d >> 5) & 0x3F);
                b += (a & 0x1F) + (d & 0x1F);
            }
            uint16_t v = (uint16_t)(((r / 4) << 11) | ((g / 4) << 5) | (b / 4));
            out[x] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}
