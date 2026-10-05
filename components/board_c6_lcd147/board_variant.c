#include "board_variant.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_variant";

/* AXS5106L capacitive touch controller. Present only on the Touch board, which
 * is what makes it the right thing to look for: the QMI8658 at 0x6B would be
 * ambiguous, because an external QMI8658 can hang off the plain board's free
 * I2C pads. */
#define AXS5106_ADDR 0x63
#define QMI8658_ADDR 0x6B

#define PROBE_TIMEOUT_MS 50

static bool s_detected;
static board_variant_t s_variant = BOARD_VARIANT_PLAIN;
static board_pins_t s_pins;
static i2c_master_bus_handle_t s_bus;

static const board_pins_t k_pins_plain = {
    .spi_sclk = BOARD_PLAIN_PIN_SPI_SCLK,
    .spi_mosi = BOARD_PLAIN_PIN_SPI_MOSI,
    .spi_miso = BOARD_PLAIN_PIN_SPI_MISO,
    .lcd_cs = BOARD_PLAIN_PIN_LCD_CS,
    .lcd_dc = BOARD_PLAIN_PIN_LCD_DC,
    .lcd_rst = BOARD_PLAIN_PIN_LCD_RST,
    .lcd_bl = BOARD_PLAIN_PIN_LCD_BL,
    .sd_cs = BOARD_PLAIN_PIN_SD_CS,
    .button = BOARD_PLAIN_PIN_BUTTON,
    .tp_rst = GPIO_NUM_NC,
    .tp_int = GPIO_NUM_NC,
};

static const board_pins_t k_pins_touch = {
    .spi_sclk = BOARD_TOUCH_PIN_SPI_SCLK,
    .spi_mosi = BOARD_TOUCH_PIN_SPI_MOSI,
    .spi_miso = BOARD_TOUCH_PIN_SPI_MISO,
    .lcd_cs = BOARD_TOUCH_PIN_LCD_CS,
    .lcd_dc = BOARD_TOUCH_PIN_LCD_DC,
    .lcd_rst = BOARD_TOUCH_PIN_LCD_RST,
    .lcd_bl = BOARD_TOUCH_PIN_LCD_BL,
    .sd_cs = BOARD_TOUCH_PIN_SD_CS,
    .button = BOARD_TOUCH_PIN_BUTTON,
    .tp_rst = BOARD_TOUCH_PIN_TP_RST,
    .tp_int = BOARD_TOUCH_PIN_TP_INT,
};

/* The touch controller holds its I2C interface down while its reset line is
 * asserted, and nothing drives that line until we do -- so a chip that came up
 * held in reset would look exactly like an absent one. Only worth the pulse
 * when the cheap probe was inconclusive: on the plain board TP_RST is a free
 * pad, and driving free pads on a board we have not identified yet is a thing
 * to do reluctantly and once. */
static void pulse_touch_reset(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_PIN_TP_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return;
    }
    gpio_set_level(BOARD_TOUCH_PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_TOUCH_PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static bool probe(uint8_t addr)
{
    return s_bus && i2c_master_probe(s_bus, addr, PROBE_TIMEOUT_MS) == ESP_OK;
}

esp_err_t board_variant_detect(void)
{
    if (s_detected) {
        return ESP_OK;
    }
    s_detected = true; /* set first: every path below settles on a variant */

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        /* Without the bus there is no evidence either way, and the plain board
         * is the variant a silent bus is indistinguishable from. Say so
         * loudly: on a Touch board this means a blank screen, because the SPI
         * pins we are about to bring up will be the wrong ones. */
        ESP_LOGE(TAG, "I2C bus init failed (%s) -- assuming %s; if this board HAS a touch "
                      "screen, the display will not come up",
                 esp_err_to_name(err), "C6-LCD-1.47");
        s_bus = NULL;
        s_variant = BOARD_VARIANT_PLAIN;
        s_pins = k_pins_plain;
        return err;
    }

    bool touch = probe(AXS5106_ADDR);
    if (!touch && probe(QMI8658_ADDR)) {
        /* An IMU but no touch controller: either a Touch board whose panel is
         * held in reset, or a plain board with an external IMU wired to the
         * same pads. One pulse tells them apart. */
        ESP_LOGI(TAG, "IMU present but no touch controller; pulsing TP_RST and retrying");
        pulse_touch_reset();
        touch = probe(AXS5106_ADDR);
    }

    s_variant = touch ? BOARD_VARIANT_TOUCH : BOARD_VARIANT_PLAIN;
    s_pins = touch ? k_pins_touch : k_pins_plain;

    ESP_LOGI(TAG, "detected %s (touch controller %s at 0x%02X)", board_variant_name(),
             touch ? "found" : "absent", AXS5106_ADDR);
    return ESP_OK;
}

board_variant_t board_variant(void)
{
    board_variant_detect();
    return s_variant;
}

const char *board_variant_name(void)
{
    board_variant_detect();
    return s_variant == BOARD_VARIANT_TOUCH ? "C6-Touch-LCD-1.47" : "C6-LCD-1.47";
}

bool board_has_touch(void)
{
    return board_variant() == BOARD_VARIANT_TOUCH;
}

const board_pins_t *board_pins(void)
{
    board_variant_detect();
    return &s_pins;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    board_variant_detect();
    return s_bus;
}
