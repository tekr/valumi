/* Internal: QMI8658 register-level driver, used only by board_imu.c. */
#pragma once

#include <stdbool.h>
#include "board_imu.h"
#include "driver/i2c_master.h"

/**
 * @brief Probe for a QMI8658 on @p bus and configure it if present.
 *
 * @return ESP_OK if the part answered with the expected WHO_AM_I.
 */
esp_err_t qmi8658_probe(i2c_master_bus_handle_t bus);

/** Read acceleration in g. Returns false if no new sample is ready. */
bool qmi8658_read_accel(board_accel_t *out);
