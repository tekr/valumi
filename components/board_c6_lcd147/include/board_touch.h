/*
 * Capacitive touch for the ESP32-C6-Touch-LCD-1.47 (AXS5106L).
 *
 * Poll-based, like board_button: call board_touch_poll() from the render loop
 * and it returns the current contact plus any gesture that just completed.
 * Everything is reported in the display's LOGICAL coordinates -- the same ones
 * the drawing routines take -- including through a 180-degree flip, so callers
 * never see panel-native axes.
 *
 * On the plain board every call is a cheap no-op reporting "no touch", so an
 * app can call this unconditionally and simply never see a gesture.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_TOUCH_NONE = 0,
    BOARD_TOUCH_TAP,         /* pressed and released without travelling far */
    BOARD_TOUCH_SWIPE_LEFT,  /* finger moved toward decreasing x */
    BOARD_TOUCH_SWIPE_RIGHT,
} board_touch_gesture_t;

typedef struct {
    /* True while a finger is on the glass. Reported every poll, so it can gate
     * things that should pause under a fingertip. */
    bool down;
    /* Position of the current contact, or of the last one while down is false.
     * Logical display coordinates. */
    int x;
    int y;
    /* Set on the poll where a gesture completed, i.e. on release. Reported
     * once. */
    board_touch_gesture_t gesture;
} board_touch_state_t;

/**
 * @brief Bring up the touch controller.
 *
 * @return ESP_ERR_NOT_SUPPORTED on the plain board -- not an error worth
 *         aborting for; poll() then reports no contact forever.
 */
esp_err_t board_touch_init(void);

/**
 * @brief Sample the panel.
 *
 * Call at 10 Hz or faster; a swipe lasts a couple of hundred milliseconds and
 * is measured between the samples that see it start and end.
 *
 * @return false if @p out is untouched (no panel, or the read failed).
 */
bool board_touch_poll(board_touch_state_t *out);

#ifdef __cplusplus
}
#endif
