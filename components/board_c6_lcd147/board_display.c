#include "board_display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "board_lcd_jd9853.h"
#include "board_variant.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board_display";

/* 80 MHz is stable on this wiring (verified on hardware; Waveshare's own demo
 * is far more conservative at 12 MHz). A full frame is 110,080 bytes, so the
 * bus costs ~11 ms per full-screen update. */
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)

#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define BL_LEDC_DUTY_MAX (1 << 10)
#define BL_LEDC_FREQ_HZ (5000)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb[2];
static int s_back;
static bool s_single_buffer;
static int s_width;
static int s_height;
static bool s_landscape;
static bool s_flipped;
static SemaphoreHandle_t s_trans_done;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_done, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static esp_err_t backlight_init(gpio_num_t bl_pin)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");

    ledc_channel_config_t channel = {
        .gpio_num = bl_pin,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight channel");
    return ESP_OK;
}

/* Push the current orientation into the panel's MADCTL and window offsets.
 *
 * The 172-wide glass sits at column 34 of the controller's 240-wide RAM. That
 * gap is applied to the physical CASET/RASET registers, so when swap_xy
 * redirects logical x to physical rows the 34-column offset has to move to the
 * y gap.
 *
 * The flip is the same two mirror bits inverted, and the gap does NOT change
 * with it: the window is centred in the frame memory ((240 - 172) / 2 on both
 * sides), so mirroring maps it onto itself. That symmetry is the only reason a
 * 180-degree turn is free here; on an off-centre panel it would need the
 * complementary gap.
 *
 * Verified against Waveshare's own factory demo, which uses exactly these
 * combinations for its 90- and 270-degree modes on the JD9853. The ST7789
 * takes the same ones. */
static esp_err_t apply_orientation(void)
{
    bool mx, my;
    int x_gap, y_gap;

    if (s_landscape) {
        mx = !s_flipped;
        my = s_flipped;
        x_gap = 0;
        y_gap = BOARD_LCD_X_GAP;
    } else {
        mx = s_flipped;
        my = s_flipped;
        x_gap = BOARD_LCD_X_GAP;
        y_gap = BOARD_LCD_Y_GAP;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, s_landscape), TAG, "swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, mx, my), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, x_gap, y_gap), TAG, "gap");
    return ESP_OK;
}

/* ESP-IDF's ST7789 driver sets no gamma or voltage registers, so the plain
 * board's glass ran on the controller's power-on curve: blacks came out
 * navy and the dark end of a gradient barely darker than its top. These
 * are the values from Waveshare's own Arduino demo for this board
 * (Display_ST7789.cpp), sent after the driver's init, and bring it close to
 * the Touch board's JD9853, which has its maker's table in its init list.
 * Its RAM-control register (0xB0) is left out: that one changes the pixel
 * byte order, and the frames here already arrive in the panel's order. */
static esp_err_t st7789_tune(void)
{
    static const struct {
        uint8_t cmd;
        uint8_t data[14];
        uint8_t len;
    } cmds[] = {
        {0xB2, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 5}, /* porch */
        {0xB7, {0x35}, 1},                         /* gate voltages */
        {0xBB, {0x35}, 1},                         /* VCOM */
        {0xC0, {0x2C}, 1},                         /* LCM control */
        {0xC2, {0x01}, 1},                         /* VDV and VRH from registers */
        {0xC3, {0x13}, 1},                         /* VRH */
        {0xC4, {0x20}, 1},                         /* VDV */
        {0xC6, {0x0F}, 1},                         /* frame rate: 60 Hz */
        {0xD0, {0xA4, 0xA1}, 2},                   /* power control */
        {0xD6, {0xA1}, 1},
        {0xE0, {0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30},
         14}, /* positive gamma */
        {0xE1, {0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32},
         14}, /* negative gamma */
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_io, cmds[i].cmd, cmds[i].data, cmds[i].len),
                            TAG, "st7789 0x%02X", cmds[i].cmd);
    }
    return ESP_OK;
}

esp_err_t board_display_init(void)
{
    board_display_cfg_t cfg = {0};
    return board_display_init_ex(&cfg);
}

esp_err_t board_display_init_ex(const board_display_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_panel == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialised");
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "null cfg");

    /* Settle which board this is before a single pin is configured: the two
     * variants share almost no pins, and picking the wrong set gives a dark
     * screen with no error anywhere. */
    board_variant_detect();
    const board_pins_t *pins = board_pins();

    s_single_buffer = cfg->single_buffer;
    s_landscape = cfg->landscape;
    s_flipped = false;
    s_width = cfg->landscape ? BOARD_LCD_V_RES : BOARD_LCD_H_RES;
    s_height = cfg->landscape ? BOARD_LCD_H_RES : BOARD_LCD_V_RES;

    s_trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_trans_done, ESP_ERR_NO_MEM, TAG, "no mem for semaphore");
    if (!s_single_buffer) {
        /* Pipelined mode: nothing is in flight yet, so the first present()
         * must not block. Synchronous mode instead waits on the transfer it
         * just queued, so it must NOT start pre-given. */
        xSemaphoreGive(s_trans_done);
    }

    /* MISO is wired for the TF card; the LCD itself is write-only. Sharing the
     * bus is fine because the two devices have separate CS lines. */
    spi_bus_config_t buscfg = {
        .sclk_io_num = pins->spi_sclk,
        .mosi_io_num = pins->spi_mosi,
        .miso_io_num = pins->spi_miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* Large enough to push a whole frame as a single transfer, so the
         * completion callback fires exactly once per present(). */
        .max_transfer_sz = BOARD_FB_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init");

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = pins->lcd_cs,
        .dc_gpio_num = pins->lcd_dc,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_config, &s_io),
        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = pins->lcd_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BOARD_LCD_BITS_PER_PIXEL,
    };
    /* ST7789 ships with ESP-IDF; the Touch board's JD9853 does not, so that one
     * is vendored in this component. Both hand back the same esp_lcd_panel_t
     * interface, so everything below here is identical. */
    if (board_has_touch()) {
        ESP_RETURN_ON_ERROR(board_lcd_new_panel_jd9853(s_io, &panel_config, &s_panel), TAG,
                            "jd9853 panel");
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel), TAG,
                            "st7789 panel");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    if (!board_has_touch()) {
        ESP_RETURN_ON_ERROR(st7789_tune(), TAG, "st7789 tune");
    }
    /* This panel ships inverted; without this everything is a photo negative. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");

    ESP_RETURN_ON_ERROR(apply_orientation(), TAG, "orientation");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on");

    int fb_count = s_single_buffer ? 1 : 2;
    for (int i = 0; i < fb_count; i++) {
        s_fb[i] = heap_caps_malloc(BOARD_FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_fb[i] == NULL) {
            ESP_LOGE(TAG,
                     "framebuffer %d (%d bytes) did not fit; largest free DMA block is %u bytes",
                     i, BOARD_FB_BYTES,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            return ESP_ERR_NO_MEM;
        }
        memset(s_fb[i], 0, BOARD_FB_BYTES);
    }
    s_back = 0;

    ESP_RETURN_ON_ERROR(backlight_init(pins->lcd_bl), TAG, "backlight");

    ESP_LOGI(TAG, "%s: %dx%d @ %d MHz, %d x %d byte framebuffer%s, %u bytes internal RAM left",
             board_variant_name(), s_width, s_height, LCD_PIXEL_CLOCK_HZ / 1000000, fb_count,
             BOARD_FB_BYTES, fb_count == 1 ? "" : "s",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

int board_display_width(void)
{
    return s_width;
}

int board_display_height(void)
{
    return s_height;
}

uint16_t *board_display_back_buffer(void)
{
    return s_fb[s_back];
}

void board_display_present(void)
{
    if (s_single_buffer) {
        /* Queue and wait for completion: the caller may scribble on the one
         * and only buffer as soon as we return. */
        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_width, s_height, s_fb[0]);
        xSemaphoreTake(s_trans_done, portMAX_DELAY);
        return;
    }

    /* Wait for the previous frame to leave the DMA engine before touching the
     * queue again; the buffer it was reading becomes our next back buffer. */
    xSemaphoreTake(s_trans_done, portMAX_DELAY);

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, s_width, s_height, s_fb[s_back]);
    s_back ^= 1;
}

void board_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    uint32_t duty = (percent * (BL_LEDC_DUTY_MAX - 1)) / 100;
    ESP_ERROR_CHECK(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL));
}

void board_display_set_flipped(bool flipped)
{
    if (s_panel == NULL || flipped == s_flipped) {
        return;
    }
    s_flipped = flipped;
    esp_err_t err = apply_orientation();
    if (err != ESP_OK) {
        /* Leaving the flag set to something the panel did not accept would
         * make every later call a no-op against a stale belief, so put it
         * back. */
        s_flipped = !flipped;
        ESP_LOGW(TAG, "could not flip the panel: %s", esp_err_to_name(err));
    }
}

bool board_display_flipped(void)
{
    return s_flipped;
}
