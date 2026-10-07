/*
 * Minimal software drawing primitives for a 16bpp framebuffer.
 *
 * IMPORTANT: pixels are stored in the byte order the JD9853 expects on the
 * wire (high byte first), not in native little-endian RGB565. Always build
 * colours with board_rgb565() rather than packing them by hand, otherwise the
 * red and blue channels come out scrambled.
 */
#pragma once

#include <stdint.h>
#include "board_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Pack an RGB triplet into a panel-ready RGB565 value.
 */
static inline uint16_t board_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8)); /* panel wants big-endian */
}

/**
 * @brief Confine drawing to a window; what falls outside it is dropped.
 *
 * Honoured by every primitive here and by board_font_text(), but not by
 * board_gfx_clear(), the rotate blit or the shrink, which own whole frames.
 * It stays set until reset, so reset it before anyone else draws.
 */
void board_gfx_set_clip(int x, int y, int w, int h);
void board_gfx_reset_clip(void);
/** The window in force, already cut to the screen: x0 <= x < x1, y0 <= y < y1. */
void board_gfx_get_clip(int *x0, int *y0, int *x1, int *y1);

/** Fill the whole framebuffer with one colour. */
void board_gfx_clear(uint16_t *fb, uint16_t color);

/** Set a single pixel; outside the clip window (or the screen) it is dropped. */
void board_gfx_pixel(uint16_t *fb, int x, int y, uint16_t color);

/** Filled axis-aligned rectangle, cut to the clip window. */
void board_gfx_fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);

/** One-pixel-wide rectangle outline. */
void board_gfx_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t color);

void board_gfx_hline(uint16_t *fb, int x, int y, int w, uint16_t color);
void board_gfx_vline(uint16_t *fb, int x, int y, int h, uint16_t color);

/** Blend @p color over the existing pixel at opacity @p alpha (0-255). */
void board_gfx_blend_pixel(uint16_t *fb, int x, int y, uint16_t color, uint8_t alpha);

/** Darken a rectangle toward black: @p keep (0-255) of each pixel is left. */
void board_gfx_dim_rect(uint16_t *fb, int x, int y, int w, int h, uint8_t keep);

/** Filled circle centred on (cx, cy). */
void board_gfx_fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t color);

/**
 * @brief Draw text in the built-in 5x7 font.
 *
 * Covers ASCII 0x20-0x5F; lowercase is folded to uppercase. Each glyph cell is
 * 6x8 pixels before scaling, so `scale` 2 gives 12x16. Returns the x coordinate
 * just past the last glyph.
 */
int board_gfx_text(uint16_t *fb, int x, int y, const char *str, uint16_t color, int scale);

/**
 * @brief Rotate an image about the screen centre into the framebuffer.
 *
 * @p src is a @p src_w by @p src_h picture of the WHOLE screen -- usually a
 * captured frame, optionally shrunk -- and is stretched back to full size as
 * it is rotated. @p dst is a full logical framebuffer and must not overlap it.
 *
 * The image is scaled down as it turns so that no part of it leaves the
 * screen: a 320x172 frame fits at only 54% when square-on at 90 degrees.
 * Anything the rotated image does not cover is filled with @p bg. At 0 and PI
 * the fit scale is back to 1.
 *
 * That shrink is what makes a half-size source worth having: through the
 * middle of a turn the image is drawn smaller than the screen anyway, so the
 * detail a full-size copy would carry is being thrown away regardless -- and
 * a half-size copy costs a quarter of the memory, which is the difference
 * between fitting alongside a live TLS session and not.
 *
 * Nearest-neighbour, fixed point, one pass: about 5 ms for a 320x172
 * destination on a 160 MHz C6, whatever the source size.
 */
void board_gfx_rotate_blit(uint16_t *dst, const uint16_t *src, int src_w, int src_h,
                           float radians, uint16_t bg);

/**
 * @brief Box-filter a full framebuffer down to half size in each axis.
 *
 * @p dst must hold (width/2) * (height/2) pixels. Averaging rather than
 * dropping pixels matters here: dropped pixels alias thin strokes and text
 * edges, which vanish and reappear as the image turns and read as flicker
 * rather than as softness.
 */
void board_gfx_shrink_half(uint16_t *dst, const uint16_t *src);

#define BOARD_GFX_GLYPH_W 6
#define BOARD_GFX_GLYPH_H 8

#ifdef __cplusplus
}
#endif
