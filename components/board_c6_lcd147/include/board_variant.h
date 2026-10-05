/*
 * Which of the two 1.47" C6 boards are we running on?
 *
 * They differ in almost every pin and in the LCD controller, so this has to be
 * settled before the display comes up. Detection is a single I2C probe on
 * GPIO18/19 -- the one bus that IS the same on both -- for the AXS5106L touch
 * controller, which exists only on the Touch board.
 *
 * Every entry point in this component detects on first use, so callers that do
 * not care need not call anything. Callers that do
 * care can force it early and log the result.
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_VARIANT_PLAIN = 0, /* ESP32-C6-LCD-1.47: ST7789, no touch, no IMU */
    BOARD_VARIANT_TOUCH,     /* ESP32-C6-Touch-LCD-1.47: JD9853, AXS5106L, QMI8658 */
} board_variant_t;

/**
 * @brief Detect the board, if it has not been detected already.
 *
 * Idempotent and safe to call from anywhere.
 *
 * A variant is always selected, even on failure: if the I2C bus cannot be
 * brought up there is no evidence either way, so the plain board is assumed --
 * that being the variant a silent bus is indistinguishable from -- and the bus
 * error is returned for information. Do NOT ESP_ERROR_CHECK this: the fallback
 * is the useful behaviour, and aborting would turn a recoverable bus glitch on
 * a plain board into a dead one.
 */
esp_err_t board_variant_detect(void);

board_variant_t board_variant(void);

/** "C6-LCD-1.47" or "C6-Touch-LCD-1.47". */
const char *board_variant_name(void);

/** True on the Touch board: touch panel and IMU are present. */
bool board_has_touch(void);

/** Resolved pin map for the detected board. Never NULL. */
const board_pins_t *board_pins(void);

/**
 * @brief The shared I2C master bus on GPIO18/19.
 *
 * Created during detection and shared by the touch controller and the IMU --
 * two devices on one bus, so neither may create its own. NULL if the bus could
 * not be created.
 */
i2c_master_bus_handle_t board_i2c_bus(void);

#ifdef __cplusplus
}
#endif
