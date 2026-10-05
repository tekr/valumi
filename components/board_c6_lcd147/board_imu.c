#include "board_imu.h"
#include "board_imu_qmi8658.h"
#include "board_pins.h"
#include "board_variant.h"

#include <math.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "board_imu";

static board_imu_source_t s_source = BOARD_IMU_SOURCE_NONE;

/* ---- Simulated source -------------------------------------------------- */

/* Sweeps gravity slowly around the screen and periodically fakes a shake, so
 * the physics can be exercised end to end with no sensor attached. */
static void simulated_read(board_accel_t *out)
{
    float t = (float)esp_timer_get_time() / 1000000.0f;

    /* One full rotation every ~13 s. */
    float angle = t * 0.48f;
    float gx = sinf(angle);
    float gy = cosf(angle);

    /* A 0.4 s burst of jitter every 5 s, roughly the magnitude of a real hand
     * shake, to exercise the impulse path. */
    float phase = fmodf(t, 5.0f);
    if (phase < 0.4f) {
        static uint32_t rng = 2463534242u;
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        float n1 = (float)(rng & 0xFFFF) / 32768.0f - 1.0f;
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        float n2 = (float)(rng & 0xFFFF) / 32768.0f - 1.0f;
        gx += n1 * 1.8f;
        gy += n2 * 1.8f;
    }

    out->x = gx;
    out->y = gy;
    out->z = 0.15f;
}

/* ---- Public API -------------------------------------------------------- */

esp_err_t board_imu_init(void)
{
    if (s_source != BOARD_IMU_SOURCE_NONE) {
        return ESP_OK; /* idempotent: the Touch board has callers on both sides */
    }

    /* The bus belongs to the board, not to us: on the Touch board the touch
     * controller shares it, and two i2c_new_master_bus() calls on one port
     * fail the second caller. */
    i2c_master_bus_handle_t bus = board_i2c_bus();

    if (bus != NULL && qmi8658_probe(bus) == ESP_OK) {
        s_source = BOARD_IMU_SOURCE_QMI8658;
        ESP_LOGI(TAG, "using QMI8658 on SDA=%d SCL=%d", BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
        return ESP_OK;
    }

    s_source = BOARD_IMU_SOURCE_SIMULATED;
    ESP_LOGW(TAG, "no IMU detected on SDA=%d SCL=%d - using simulated gravity",
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
    return ESP_OK;
}

bool board_imu_read(board_accel_t *out)
{
    switch (s_source) {
    case BOARD_IMU_SOURCE_QMI8658:
        return qmi8658_read_accel(out);
    case BOARD_IMU_SOURCE_SIMULATED:
        simulated_read(out);
        return true;
    default:
        return false;
    }
}

board_imu_source_t board_imu_source(void)
{
    return s_source;
}

const char *board_imu_source_name(void)
{
    switch (s_source) {
    case BOARD_IMU_SOURCE_QMI8658:
        return "QMI8658";
    case BOARD_IMU_SOURCE_SIMULATED:
        return "SIM";
    default:
        return "NONE";
    }
}
