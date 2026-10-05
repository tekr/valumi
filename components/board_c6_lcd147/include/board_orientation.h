/*
 * Which way up is the board being held?
 *
 * Only the two landscape orientations are distinguished -- the display is
 * 320x172 and the apps that care are landscape dashboards, so the question is
 * simply whether the picture needs turning through 180 degrees to stay the
 * right way up.
 *
 * Needs a real accelerometer. board_imu falls back to a simulated gravity
 * vector that sweeps in a circle, which would spin the screen forever, so this
 * module reports "unavailable" rather than using it.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the IMU for orientation sensing.
 *
 * @return ESP_ERR_NOT_SUPPORTED if there is no real accelerometer, in which
 *         case board_orientation_flipped() stays false forever and callers can
 *         simply ignore the whole feature.
 */
esp_err_t board_orientation_init(void);

bool board_orientation_available(void);

/**
 * @brief Should the display be flipped 180 degrees to stay upright?
 *
 * Samples the accelerometer at its own pace, so it is cheap to call from a
 * render loop. The answer only changes when the board has been held
 * convincingly the other way up for a moment -- see the implementation for why
 * both the angle and the dwell matter.
 */
bool board_orientation_flipped(void);

#ifdef __cplusplus
}
#endif
