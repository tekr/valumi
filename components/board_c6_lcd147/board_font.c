#include "board_font.h"

#include <string.h>

#include "board_display.h"
#include "board_gfx.h"

/* Framebuffer pixels are RGB565 stored byte-swapped (panel wire order), so
 * blending unswaps, lerps each channel, and reswaps. */
static inline uint16_t blend565_swapped(uint16_t bg_swapped, uint16_t fg_swapped, uint8_t alpha16)
{
    if (alpha16 == 0) {
        return bg_swapped;
    }
    if (alpha16 >= 15) {
        return fg_swapped;
    }

    uint16_t bg = (uint16_t)((bg_swapped >> 8) | (bg_swapped << 8));
    uint16_t fg = (uint16_t)((fg_swapped >> 8) | (fg_swapped << 8));

    /* 0..15 to an opacity out of 256, so the lerp is a shift. */
    int a = alpha16 * 17 + 1;
    int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
    r += ((int)((fg >> 11) & 0x1F) - r) * a >> 8;
    g += ((int)((fg >> 5) & 0x3F) - g) * a >> 8;
    b += ((int)(fg & 0x1F) - b) * a >> 8;

    uint16_t out = (uint16_t)((r << 11) | (g << 5) | b);
    return (uint16_t)((out >> 8) | (out << 8));
}

static const board_glyph_t *lookup(const board_font_t *font, char c)
{
    const char *pos = strchr(font->charset, c);
    if (pos == NULL && c >= 'a' && c <= 'z') {
        pos = strchr(font->charset, (char)(c - 32));
    }
    if (pos == NULL) {
        return NULL;
    }
    return &font->glyphs[pos - font->charset];
}

int board_font_text(uint16_t *fb, int x, int y, const char *str, const board_font_t *font,
                    uint16_t color)
{
    int screen_w = board_display_width();
    int cx0, cy0, cx1, cy1;
    board_gfx_get_clip(&cx0, &cy0, &cx1, &cy1);

    for (const char *p = str; *p; p++) {
        const board_glyph_t *g = lookup(font, *p);
        if (g == NULL) {
            continue;
        }

        const uint8_t *bits = font->bitmap + g->offset;
        int row_bytes = (g->w + 1) / 2;
        int gx0 = x + g->bearing_x;
        int gy0 = y + g->bearing_y;
        /* Only the part of the glyph inside the clip window is walked, so
         * text that has slid off the screen costs nothing. */
        int row0 = gy0 < cy0 ? cy0 - gy0 : 0;
        int row1 = gy0 + g->h > cy1 ? cy1 - gy0 : g->h;
        int col0 = gx0 < cx0 ? cx0 - gx0 : 0;
        int col1 = gx0 + g->w > cx1 ? cx1 - gx0 : g->w;

        for (int row = row0; row < row1; row++) {
            const uint8_t *rp = bits + row * row_bytes;
            uint16_t *out = fb + (gy0 + row) * screen_w;
            for (int col = col0; col < col1; col++) {
                int px = gx0 + col;
                uint8_t a = (col & 1) ? (rp[col / 2] & 0x0F) : (rp[col / 2] >> 4);
                if (a) {
                    out[px] = blend565_swapped(out[px], color, a);
                }
            }
        }
        x += g->advance;
    }
    return x;
}

int board_font_text_width(const char *str, const board_font_t *font)
{
    int w = 0;
    for (const char *p = str; *p; p++) {
        const board_glyph_t *g = lookup(font, *p);
        if (g) {
            w += g->advance;
        }
    }
    return w;
}
