#include "board_touch.h"
#include "board_display.h"
#include "board_pins.h"
#include "board_variant.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_touch";

#define AXS5106_ADDR 0x63

/* Reading register 0x01 returns a 14-byte block: [0] header, [1] contact count
 * in the low nibble, then six bytes per contact. Within a contact the x and y
 * high nibbles ride in the top of their own byte, which is why each coordinate
 * masks 0x0F before shifting. Two contacts are reported; we use the first. */
#define REG_TOUCH_DATA 0x01
#define TOUCH_DATA_LEN 14

/* Register 0x08 returns three identity bytes; a zero first byte means the part
 * is not really answering, which is how Waveshare's own code tests it. */
#define REG_ID 0x08

#define I2C_TIMEOUT_MS 50

/* The datasheet gives no reset timing and the two Waveshare drivers disagree:
 * their ESP-IDF BSP waits 10 ms either side, their Arduino library 200 ms then
 * 300 ms. The short version leaves this part ACKing its address but NACKing
 * every register read, which is the confusing failure this avoids. Half a
 * second, once, at boot. */
#define RESET_LOW_MS 200
#define RESET_SETTLE_MS 300

/* What separates a swipe from a tap. The screen is 320 logical pixels wide, so
 * 45 is about a seventh of it: far enough that it cannot be a shaky tap, short
 * enough for a flick with one thumb. The time limit is what stops a slow drag
 * -- someone resting a finger and gradually moving it -- from paging. */
#define SWIPE_MIN_DX 45
#define SWIPE_MAX_MS 700
#define TAP_MAX_TRAVEL 20
#define TAP_MAX_MS 500

static bool s_available;
static i2c_master_dev_handle_t s_dev;

/* Contact tracking, all in logical coordinates. */
static bool s_down;
static int s_x, s_y;
static int s_start_x, s_start_y;
static int64_t s_start_us;
static int s_travel; /* furthest distance from the start seen during this contact */

/* Two transactions, not one combined transmit_receive.
 *
 * This part will ACK its address and then NACK a repeated START -- so it looks
 * present to a bus scan and dead to every read. Waveshare's Arduino driver
 * writes the register, ends the transmission (STOP), and only then reads;
 * their own ESP-IDF driver does the same with i2c_master_transmit() followed
 * by i2c_master_receive(). Do not fold these back together.
 *
 * The timeout argument is milliseconds, not ticks. */
static esp_err_t read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    /* Deliberately silent. This runs from the render loop at up to 50 Hz, and
     * one ESP_LOG per failure would be a log storm against an undrained USB
     * console -- which blocks the render task about 100 ms a time and would
     * turn a flaky bus into a frozen screen. Callers decide what to say. */
    esp_err_t err = i2c_master_transmit(s_dev, &reg, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(s_dev, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t read_touch(uint8_t *buf)
{
    return read_reg(REG_TOUCH_DATA, buf, TOUCH_DATA_LEN);
}

esp_err_t board_touch_init(void)
{
    if (!board_has_touch()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_available) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        ESP_LOGE(TAG, "no I2C bus");
        return ESP_ERR_INVALID_STATE;
    }

    const board_pins_t *pins = board_pins();

    /* The reset line may already have been pulsed during board detection; do it
     * again unconditionally so a panel that woke up confused gets a clean
     * start, and so this works when detection took the short path. */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << pins->tp_rst,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "tp reset gpio");
    gpio_set_level(pins->tp_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(RESET_LOW_MS));
    gpio_set_level(pins->tp_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(RESET_SETTLE_MS));

    /* INT is an input we deliberately do not use. The controller drives it on
     * contact, but the read below costs ~400 us at 400 kHz and the render loop
     * polls at tens of hertz, so gating on the pin would save nothing worth the
     * extra state -- and a level-versus-pulse mistake there would silently drop
     * releases. Configured anyway so the pin is not left floating. */
    if (pins->tp_int != GPIO_NUM_NC) {
        gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << pins->tp_int,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_cfg), TAG, "tp int gpio");
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXS5106_ADDR,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "add device");

    uint8_t id[3] = {0};
    esp_err_t err = read_reg(REG_ID, id, sizeof(id));
    if (err != ESP_OK || id[0] == 0) {
        ESP_LOGE(TAG, "AXS5106L did not answer after reset: %s, id %02X %02X %02X",
                 esp_err_to_name(err), id[0], id[1], id[2]);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    s_available = true;
    ESP_LOGI(TAG, "AXS5106L ready at 0x%02X, id %02X %02X %02X", AXS5106_ADDR, id[0], id[1],
             id[2]);
    return ESP_OK;
}

/* Panel-native (x right across the narrow edge, y down the long one) into the
 * logical landscape frame the drawing code uses.
 *
 * The unflipped landscape orientation is Waveshare's "rotation 90": the
 * display gets swap_xy with mirror_x, and the touch panel needs a plain axis
 * swap to match. Flipping the display 180 degrees inverts both mirror bits, so
 * the touch mapping inverts both logical axes -- which is just the same
 * rotation applied to the finger. */
static void to_logical(int px, int py, int *lx, int *ly)
{
    *lx = py;
    *ly = px;

    if (board_display_flipped()) {
        *lx = BOARD_LCD_V_RES - 1 - *lx;
        *ly = BOARD_LCD_H_RES - 1 - *ly;
    }
}

static board_touch_gesture_t classify_release(int64_t now)
{
    int dx = s_x - s_start_x;
    int dy = s_y - s_start_y;
    int64_t ms = (now - s_start_us) / 1000;

    if (abs(dx) >= SWIPE_MIN_DX && abs(dx) > abs(dy) && ms <= SWIPE_MAX_MS) {
        return dx > 0 ? BOARD_TOUCH_SWIPE_RIGHT : BOARD_TOUCH_SWIPE_LEFT;
    }
    /* Travel, not the start-to-end displacement: a finger that wandered out
     * and came back is not a tap, however tidy its endpoints look. */
    if (s_travel <= TAP_MAX_TRAVEL && ms <= TAP_MAX_MS) {
        return BOARD_TOUCH_TAP;
    }
    return BOARD_TOUCH_NONE;
}

bool board_touch_poll(board_touch_state_t *out)
{
    if (!s_available || out == NULL) {
        return false;
    }

    uint8_t buf[TOUCH_DATA_LEN];
    if (read_touch(buf) != ESP_OK) {
        /* A dropped read must not be read as a release: that would fire a
         * spurious gesture and un-pause anything gated on the contact. Report
         * the contact we last believed in and wait for the next sample. */
        out->down = s_down;
        out->x = s_x;
        out->y = s_y;
        out->gesture = BOARD_TOUCH_NONE;
        return true;
    }

    int64_t now = esp_timer_get_time();
    int contacts = buf[1] & 0x0F;

    out->gesture = BOARD_TOUCH_NONE;

    if (contacts > 0) {
        int px = ((int)(buf[2] & 0x0F) << 8) | buf[3];
        int py = ((int)(buf[4] & 0x0F) << 8) | buf[5];
        to_logical(px, py, &s_x, &s_y);

        if (!s_down) {
            s_down = true;
            s_start_x = s_x;
            s_start_y = s_y;
            s_start_us = now;
            s_travel = 0;
        } else {
            int d = abs(s_x - s_start_x) + abs(s_y - s_start_y);
            if (d > s_travel) {
                s_travel = d;
            }
        }
    } else if (s_down) {
        s_down = false;
        out->gesture = classify_release(now);
    }

    out->down = s_down;
    out->x = s_x;
    out->y = s_y;
    return true;
}
