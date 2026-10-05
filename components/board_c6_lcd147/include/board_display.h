/*
 * Display bring-up for both 1.47" C6 boards.
 *
 * The board is detected here (see board_variant.h), so the pin map and the
 * panel driver -- ST7789 or JD9853 -- are chosen for you. Geometry is the same
 * either way, so nothing above this layer needs to know which one it got.
 *
 * Two usage patterns:
 *
 * Animation (portrait, double-buffered, pipelined DMA):
 *
 *     board_display_init();
 *     for (;;) {
 *         uint16_t *fb = board_display_back_buffer();
 *         draw_everything(fb);
 *         board_display_present();   // queues DMA, overlaps with next draw
 *     }
 *
 * Dashboard (landscape, single buffer -- frees 110 KB for Wi-Fi/TLS):
 *
 *     board_display_init_ex(&(board_display_cfg_t){
 *         .landscape = true,
 *         .single_buffer = true,
 *     });
 *     ...
 *     board_display_present();       // synchronous: returns when shifted out
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes in one full-screen 16bpp framebuffer: 172 * 320 * 2 = 110,080.
 * Orientation does not change the total. */
#define BOARD_FB_PIXELS (BOARD_LCD_H_RES * BOARD_LCD_V_RES)
#define BOARD_FB_BYTES (BOARD_FB_PIXELS * 2)

typedef struct {
    /* false: 172x320, USB at the bottom. true: 320x172. */
    bool landscape;
    /* true allocates one framebuffer instead of two and makes present()
     * synchronous. Halves the RAM cost; the right choice whenever the frame
     * rate is not the point (dashboards, tickers). */
    bool single_buffer;
} board_display_cfg_t;

/** Portrait, double-buffered. Equivalent to init_ex with a zeroed config. */
esp_err_t board_display_init(void);

esp_err_t board_display_init_ex(const board_display_cfg_t *cfg);

/** Logical width/height for the orientation chosen at init. */
int board_display_width(void);
int board_display_height(void);

/**
 * @brief The buffer the CPU may draw into right now.
 */
uint16_t *board_display_back_buffer(void);

/**
 * @brief Push the back buffer to the panel.
 *
 * Double-buffered: queues the DMA transfer, waits only for the *previous*
 * frame, then swaps -- the CPU draws frame N+1 while frame N shifts out.
 * Single-buffer: fully synchronous; returns once the panel has the frame.
 */
void board_display_present(void);

/** Backlight level, 0-100 percent, via LEDC PWM. */
void board_display_set_brightness(uint8_t percent);

/**
 * @brief Turn the picture through 180 degrees.
 *
 * Free: it is two bits in the panel's MADCTL register, not a redraw. The
 * framebuffer, the logical width and height, and every drawing routine are
 * unchanged -- only the direction the controller scans it out. Both landscape
 * orientations therefore cost exactly the same.
 *
 * Takes effect on the NEXT present(). Call it between frames; in
 * double-buffered mode a frame already queued for DMA lands in the old
 * orientation.
 */
void board_display_set_flipped(bool flipped);

bool board_display_flipped(void);

#ifdef __cplusplus
}
#endif
