/*
 * Pin maps for the two Waveshare 1.47" ESP32-C6 boards.
 *
 * These share a product name, a screen size and nothing else:
 *
 *                    ESP32-C6-LCD-1.47      ESP32-C6-Touch-LCD-1.47
 *   MCU              C6FH4, 4 MB flash      C6FH8, 8 MB flash
 *   Controller       ST7789                 JD9853
 *   SPI SCLK/MOSI    7 / 6                  1 / 2
 *   LCD RST / BL     21 / 22                22 / 23
 *   I2C devices      none                   AXS5106L touch, QMI8658 IMU
 *
 * Which one is attached is decided at runtime -- see board_variant.h -- so
 * both maps are compiled in and the resolved set lives in a board_pins_t.
 * Nothing outside the component should reference the per-variant macros
 * directly; ask board_pins() instead.
 *
 * Both panels are 1.47" IPS 172x320 with the visible window inset 34 columns
 * into a 240-wide frame memory, so geometry, framebuffer size and every
 * drawing routine are shared. Neither part has PSRAM: every framebuffer comes
 * out of the 512 KB of internal SRAM, so budget carefully (172*320*2 =
 * 110,080 bytes each).
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "hal/spi_types.h"

/* ---- Geometry, identical on both boards -------------------------------- */
#define BOARD_LCD_H_RES 172
#define BOARD_LCD_V_RES 320

/* The controller's RAM is 240 wide but the glass is only 172, so the visible
 * window starts 34 columns in ((240 - 172) / 2). Being centred is what lets a
 * 180-degree flip keep the same gap -- see board_display_set_flipped(). */
#define BOARD_LCD_X_GAP 34
#define BOARD_LCD_Y_GAP 0

#define BOARD_LCD_BITS_PER_PIXEL 16

/* SPI is shared with the TF card slot (separate CS lines) on both boards. */
#define BOARD_LCD_SPI_HOST SPI2_HOST

/* ---- I2C --------------------------------------------------------------- */
/* Same pins on both, which is what makes runtime detection cheap: probing for
 * the touch controller needs no board-specific setup. On the Touch board these
 * carry the AXS5106L and the QMI8658; on the plain board they are free pads,
 * free for an external IMU. */
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_PIN_I2C_SDA GPIO_NUM_18
#define BOARD_PIN_I2C_SCL GPIO_NUM_19
#define BOARD_I2C_FREQ_HZ 400000

/* ---- Per-variant pins -------------------------------------------------- */
/* ESP32-C6-LCD-1.47 (no touch). */
#define BOARD_PLAIN_PIN_SPI_SCLK GPIO_NUM_7
#define BOARD_PLAIN_PIN_SPI_MOSI GPIO_NUM_6
#define BOARD_PLAIN_PIN_SPI_MISO GPIO_NUM_5 /* TF card only; the LCD is write-only */
#define BOARD_PLAIN_PIN_LCD_CS GPIO_NUM_14
#define BOARD_PLAIN_PIN_LCD_DC GPIO_NUM_15
#define BOARD_PLAIN_PIN_LCD_RST GPIO_NUM_21
#define BOARD_PLAIN_PIN_LCD_BL GPIO_NUM_22
#define BOARD_PLAIN_PIN_SD_CS GPIO_NUM_4
#define BOARD_PLAIN_PIN_RGB_LED GPIO_NUM_8
/* The button labelled BOOT is a strapping pin sampled only at reset; after
 * boot it is an ordinary input. Pressed = low (external pull-up on board). */
#define BOARD_PLAIN_PIN_BUTTON GPIO_NUM_9

/* ESP32-C6-Touch-LCD-1.47. */
#define BOARD_TOUCH_PIN_SPI_SCLK GPIO_NUM_1
#define BOARD_TOUCH_PIN_SPI_MOSI GPIO_NUM_2
#define BOARD_TOUCH_PIN_SPI_MISO GPIO_NUM_3
#define BOARD_TOUCH_PIN_LCD_CS GPIO_NUM_14
#define BOARD_TOUCH_PIN_LCD_DC GPIO_NUM_15
#define BOARD_TOUCH_PIN_LCD_RST GPIO_NUM_22
#define BOARD_TOUCH_PIN_LCD_BL GPIO_NUM_23
#define BOARD_TOUCH_PIN_SD_CS GPIO_NUM_4
/* Waveshare's product page says the BOOT button is GPIO8; their own factory
 * demo binds it to GPIO9, the same pin as the plain board. The demo wins: it
 * is code that ran on the hardware, and a datasheet table is not.
 *
 * This cannot be settled in software -- an unconnected pin with a pull-up
 * reads exactly like an unpressed button -- so it was settled by pressing the
 * button and watching both pins. Redo that test, do not reason about it. */
#define BOARD_TOUCH_PIN_BUTTON GPIO_NUM_9
#define BOARD_TOUCH_PIN_TP_RST GPIO_NUM_20
#define BOARD_TOUCH_PIN_TP_INT GPIO_NUM_21
/* GPIO5 and GPIO6 carry the QMI8658's INT1/INT2. Unused here -- the app polls
 * the accelerometer at a few Hz -- but do not reassign them. */
#define BOARD_TOUCH_PIN_IMU_INT1 GPIO_NUM_5
#define BOARD_TOUCH_PIN_IMU_INT2 GPIO_NUM_6

/* ---- Misc -------------------------------------------------------------- */
/* USB-Serial-JTAG is on GPIO12 (D-) / GPIO13 (D+) on both, and is not
 * user-configurable. */

/** The pins that differ between boards, resolved once at detection. */
typedef struct {
    gpio_num_t spi_sclk;
    gpio_num_t spi_mosi;
    gpio_num_t spi_miso;
    gpio_num_t lcd_cs;
    gpio_num_t lcd_dc;
    gpio_num_t lcd_rst;
    gpio_num_t lcd_bl;
    gpio_num_t sd_cs;
    gpio_num_t button;
    /* GPIO_NUM_NC on the plain board. */
    gpio_num_t tp_rst;
    gpio_num_t tp_int;
} board_pins_t;
