/*
 * Accelerometer access: the QMI8658 on the Touch board's I2C bus, if there
 * is one.
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

/** Acceleration in g, in the sensor's own axes. */
typedef struct {
    float x;
    float y;
    float z;
} board_accel_t;

/**
 * @brief Probe for the sensor, once; later calls answer from the first.
 *
 * @return ESP_OK with a sensor, ESP_ERR_NOT_FOUND without one.
 */
esp_err_t board_imu_init(void);

bool board_imu_present(void);

/**
 * @brief Read the current acceleration.
 *
 * @return false if there is no sensor or it had no new sample ready; @p out
 *         is untouched.
 */
bool board_imu_read(board_accel_t *out);

#ifdef __cplusplus
}
#endif
