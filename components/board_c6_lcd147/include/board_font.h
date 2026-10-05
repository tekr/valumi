/*
 * Anti-aliased proportional bitmap fonts.
 *
 * Font data is generated offline by tools/gen_font.py from a TTF: each glyph
 * is 4-bit coverage, two pixels per byte. The renderer alpha-blends the text
 * colour over whatever is already in the framebuffer, so glyphs sit cleanly
 * on any background -- unlike board_gfx_text, which writes opaque pixels of
 * a blocky scaled 5x7 cell.
 *
 * Coordinates: (x, y) is the top-left of the line box; the baseline sits at
 * y + font->baseline. Text width comes from advances, so measure with
 * board_font_text_width rather than summing glyph bitmaps.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t w, h;          /* ink bitmap size, pixels; 0 for space */
    int8_t bearing_x;      /* ink left relative to pen x */
    int8_t bearing_y;      /* ink top relative to line top */
    uint8_t advance;       /* pen advance, pixels */
    uint32_t offset;       /* into the font's packed bitmap */
} board_glyph_t;

typedef struct {
    const uint8_t *bitmap;
    const board_glyph_t *glyphs;
    const char *charset;   /* glyph i renders charset[i]; lookup is strchr */
    uint8_t line_height;
    uint8_t baseline;
} board_font_t;

/**
 * @brief Draw @p str with its line box's top-left at (x, y).
 *
 * Characters missing from the font's charset are skipped. Lowercase input is
 * folded to uppercase when only uppercase is present.
 *
 * @return the pen x just past the last glyph.
 */
int board_font_text(uint16_t *fb, int x, int y, const char *str, const board_font_t *font,
                    uint16_t color);

/** Width in pixels board_font_text would advance for @p str. */
int board_font_text_width(const char *str, const board_font_t *font);

#ifdef __cplusplus
}
#endif
