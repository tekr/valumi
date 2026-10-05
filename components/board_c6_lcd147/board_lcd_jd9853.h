/*
 * Internal: JD9853 esp_lcd panel driver, used only by board_display.c.
 *
 * ESP-IDF ships an ST7789 driver but not a JD9853 one, and the Touch variant
 * of this board uses the latter. Vendored rather than pulled from the
 * component registry on purpose: the only registry component (mydazy/
 * esp_lcd_jd9853) defaults to a BOE 1.83" 240x284 panel, and its power,
 * timing and gamma tables differ from the ones Waveshare ships for the 1.47"
 * 172x320 glass on this board -- those registers are panel-specific, not
 * controller-specific, so the wrong table is a wrong picture.
 *
 * Adapted from Waveshare's ESP32-C6-Touch-LCD-1.47 factory demo, itself
 * derived from Espressif's ST7789 driver (Apache-2.0).
 */
#pragma once

#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create an esp_lcd panel for the JD9853.
 *
 * Behaves like esp_lcd_new_panel_st7789(): the returned handle supports
 * reset/init/draw_bitmap/invert_color/mirror/swap_xy/set_gap/disp_on_off, so
 * callers do not need to know which controller they have.
 */
esp_err_t board_lcd_new_panel_jd9853(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
