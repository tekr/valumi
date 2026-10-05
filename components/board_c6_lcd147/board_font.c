#include "board_font.h"

#include <string.h>

#include "board_display.h"

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

    uint32_t br = (bg >> 11) & 0x1F, bgr = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    uint32_t fr = (fg >> 11) & 0x1F, fgr = (fg >> 5) & 0x3F, fb_ = fg & 0x1F;

    uint32_t r = (fr * alpha16 + br * (15 - alpha16)) / 15;
    uint32_t g = (fgr * alpha16 + bgr * (15 - alpha16)) / 15;
    uint32_t b = (fb_ * alpha16 + bb * (15 - alpha16)) / 15;

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
    int screen_h = board_display_height();

    for (const char *p = str; *p; p++) {
        const board_glyph_t *g = lookup(font, *p);
        if (g == NULL) {
            continue;
        }

        const uint8_t *bits = font->bitmap + g->offset;
        int row_bytes = (g->w + 1) / 2;
        int gx0 = x + g->bearing_x;
        int gy0 = y + g->bearing_y;

        for (int row = 0; row < g->h; row++) {
            int py = gy0 + row;
            if ((unsigned)py >= (unsigned)screen_h) {
                continue;
            }
            const uint8_t *rp = bits + row * row_bytes;
            uint16_t *out = fb + py * screen_w;
            for (int col = 0; col < g->w; col++) {
                int px = gx0 + col;
                if ((unsigned)px >= (unsigned)screen_w) {
                    continue;
                }
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
