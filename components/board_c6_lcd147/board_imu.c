#include "board_imu.h"
#include "board_imu_qmi8658.h"
#include "board_pins.h"
#include "board_variant.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "board_imu";

static bool s_probed, s_present;

esp_err_t board_imu_init(void)
{
    if (s_probed) {
        return s_present ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    s_probed = true;

    /* The bus belongs to the board, not to us: on the Touch board the touch
     * controller shares it, and two i2c_new_master_bus() calls on one port
     * fail the second caller. */
    i2c_master_bus_handle_t bus = board_i2c_bus();
    s_present = bus != NULL && qmi8658_probe(bus) == ESP_OK;
    if (s_present) {
        ESP_LOGI(TAG, "using QMI8658 on SDA=%d SCL=%d", BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "no IMU on SDA=%d SCL=%d", BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
    return ESP_ERR_NOT_FOUND;
}

bool board_imu_present(void)
{
    return s_present;
}

bool board_imu_read(board_accel_t *out)
{
    return s_present && qmi8658_read_accel(out);
}
