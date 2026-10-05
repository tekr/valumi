#include "board_imu_qmi8658.h"
#include "board_pins.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "qmi8658";

#define QMI8658_ADDR 0x6B
#define QMI8658_WHO_AM_I_VALUE 0x05

/* Register addresses (subset; the part has many more). */
#define REG_WHO_AM_I 0
#define REG_CTRL1 2
#define REG_CTRL2 3
#define REG_CTRL3 4
#define REG_CTRL7 8
#define REG_STATUS0 46
#define REG_AX_L 53
#define REG_RESET 96

/* CTRL2 = 0x95 selects the +/-4 g full-scale range at 250 Hz, so one LSB of a
 * signed 16-bit sample is 4/32768 g. */
#define ACCEL_G_PER_LSB (4.0f / 32768.0f)

static i2c_master_dev_handle_t s_dev;

/* The trailing argument is a timeout in MILLISECONDS, not ticks -- passing
 * pdMS_TO_TICKS() here silently divides it by the tick period. */
#define I2C_TIMEOUT_MS 100

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t qmi8658_probe(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_ADDR,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "add device");

    uint8_t id = 0;
    esp_err_t err = reg_read(REG_WHO_AM_I, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "no response at 0x%02X: %s", QMI8658_ADDR, esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    if (id != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "device at 0x%02X reported WHO_AM_I 0x%02X, expected 0x%02X", QMI8658_ADDR,
                 id, QMI8658_WHO_AM_I_VALUE);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(reg_write(REG_RESET, 0xB0), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(15));
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL1, 0x40), TAG, "ctrl1"); /* auto-increment reads */
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL7, 0x03), TAG, "ctrl7"); /* enable accel + gyro */
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL2, 0x95), TAG, "ctrl2"); /* accel +/-4 g, 250 Hz */
    ESP_RETURN_ON_ERROR(reg_write(REG_CTRL3, 0xD5), TAG, "ctrl3"); /* gyro 512 dps, 250 Hz */

    ESP_LOGI(TAG, "QMI8658 found at 0x%02X, accel +/-4g @ 250 Hz", QMI8658_ADDR);
    return ESP_OK;
}

bool qmi8658_read_accel(board_accel_t *out)
{
    if (s_dev == NULL) {
        return false;
    }

    uint8_t status = 0;
    if (reg_read(REG_STATUS0, &status, 1) != ESP_OK) {
        return false;
    }
    if ((status & 0x03) == 0) {
        return false; /* no new sample since last read */
    }

    /* Samples are signed little-endian 16-bit. Reading them into an unsigned
     * type (as the vendor sample does) silently discards the sign, which shows
     * up as gravity only ever pointing one way. */
    uint8_t raw[6];
    if (reg_read(REG_AX_L, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    int16_t ax = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    int16_t ay = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    int16_t az = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));

    out->x = (float)ax * ACCEL_G_PER_LSB;
    out->y = (float)ay * ACCEL_G_PER_LSB;
    out->z = (float)az * ACCEL_G_PER_LSB;
    return true;
}
