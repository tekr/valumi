/*
 * Accelerometer access, with a simulated source for developing without one.
 *
 * This board has no onboard IMU (the ESP32-C6-*Touch*-LCD-1.47 does; this one
 * does not), so the sensor is expected on the external I2C bus. If no sensor
 * answers at boot, a simulated gravity vector is used instead and reported
 * through board_imu_source(), so an app can say so on screen rather than
 * pretending it has real data.
 *
 * A note on what the reading means: an accelerometer at rest measures the
 * reaction to gravity, and under acceleration it measures that too. In the
 * device's own frame those are indistinguishable, so the raw vector IS the
 * effective gravity the contents of the box feel. Tilting, sliding and shaking
 * all fall out of it with no separate handling.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_IMU_SOURCE_NONE = 0,
    BOARD_IMU_SOURCE_QMI8658,
    BOARD_IMU_SOURCE_SIMULATED,
} board_imu_source_t;

/** Acceleration in g, in the sensor's own axes. */
typedef struct {
    float x;
    float y;
    float z;
} board_accel_t;

/**
 * @brief Bring up the I2C bus and probe for a supported sensor.
 *
 * Always succeeds: if no sensor is found it selects the simulated source and
 * logs a warning saying so.
 */
esp_err_t board_imu_init(void);

/**
 * @brief Read the current acceleration.
 *
 * @return false if the sensor had no new sample ready; @p out is untouched.
 */
bool board_imu_read(board_accel_t *out);

board_imu_source_t board_imu_source(void);

/** Short label for on-screen display, e.g. "QMI8658" or "SIM". */
const char *board_imu_source_name(void);

#ifdef __cplusplus
}
#endif
