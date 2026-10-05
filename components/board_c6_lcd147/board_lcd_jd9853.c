#include "board_lcd_jd9853.h"

#include <stdlib.h>
#include <sys/cdefs.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jd9853";

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned delay_ms;
} jd9853_init_cmd_t;

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
} jd9853_panel_t;

/* Panel-specific bring-up, straight from Waveshare's table for this glass.
 * The 0xDF pair unlocks the vendor register space; 0xDE selects a register
 * page. Do not reorder or prune: several of these only take effect on the page
 * selected by the 0xDE before them.
 *
 * 0x2A/0x2B set the default window to columns 34..205 of the 240-wide RAM --
 * the same 34-column offset the ST7789 variant needs, for the same reason (the
 * 172-wide glass sits centred in a wider frame memory). draw_bitmap sets its
 * own window every transfer, so these matter only until the first frame. */
static const jd9853_init_cmd_t k_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120}, /* sleep out */
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xB2, (uint8_t[]){0x23}, 1, 0},
    {0xB7, (uint8_t[]){0x00, 0x47, 0x00, 0x6F}, 4, 0},
    {0xBB, (uint8_t[]){0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0}, 6, 0},
    {0xC0, (uint8_t[]){0x44, 0xA4}, 2, 0},
    {0xC1, (uint8_t[]){0x16}, 1, 0},
    {0xC3, (uint8_t[]){0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77}, 8, 0},
    /* First byte picks the frame rate: 0x00 = 60 Hz. 320 gate lines. */
    {0xC4,
     (uint8_t[]){0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79, 0x0B, 0x0A, 0x16, 0x82},
     12, 0},
    {0xC8,
     (uint8_t[]){0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26, 0x25, 0x17, 0x12,
                 0x0D, 0x04, 0x00, 0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26,
                 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00},
     32, 0}, /* gamma, red and blue */
    {0xD0, (uint8_t[]){0x04, 0x06, 0x6B, 0x0F, 0x00}, 5, 0},
    {0xD7, (uint8_t[]){0x00, 0x30}, 2, 0},
    {0xE6, (uint8_t[]){0x14}, 1, 0},
    {0xDE, (uint8_t[]){0x01}, 1, 0}, /* page 1 */
    {0xB7, (uint8_t[]){0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},
    {0xC1, (uint8_t[]){0x14, 0x15, 0xC0}, 3, 0},
    {0xC2, (uint8_t[]){0x06, 0x3A}, 2, 0},
    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xDE, (uint8_t[]){0x02}, 1, 0}, /* page 2 */
    {0xE5, (uint8_t[]){0x00, 0x02, 0x00}, 3, 0},
    {0xE5, (uint8_t[]){0x01, 0x02, 0x00}, 3, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0}, /* back to page 0 */
    {0x35, (uint8_t[]){0x00}, 1, 0}, /* tearing effect line on */
    {0x3A, (uint8_t[]){0x05}, 1, 0}, /* RGB565 */
    {0x2A, (uint8_t[]){0x00, 0x22, 0x00, 0xCD}, 4, 0}, /* columns 34..205 */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x3F}, 4, 0}, /* rows 0..319 */
    {0xDE, (uint8_t[]){0x02}, 1, 0},
    {0xE5, (uint8_t[]){0x00, 0x02, 0x00}, 3, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x29, (uint8_t[]){0x00}, 0, 0}, /* display on */
};

static esp_err_t panel_del(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);
    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t panel_reset(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);

    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, LCD_CMD_SWRESET, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(20)); /* spec: >=5 ms before the next command */
    return ESP_OK;
}

static esp_err_t panel_init(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, LCD_CMD_SLPOUT, NULL, 0), TAG, "slpout");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(p->io, LCD_CMD_MADCTL, (uint8_t[]){p->madctl_val}, 1), TAG,
        "madctl");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(p->io, LCD_CMD_COLMOD, (uint8_t[]){p->colmod_val}, 1), TAG,
        "colmod");

    for (size_t i = 0; i < sizeof(k_init_cmds) / sizeof(k_init_cmds[0]); i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, k_init_cmds[i].cmd,
                                                      k_init_cmds[i].data,
                                                      k_init_cmds[i].data_bytes),
                            TAG, "init cmd 0x%02X", k_init_cmds[i].cmd);
        if (k_init_cmds[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(k_init_cmds[i].delay_ms));
        }
    }
    return ESP_OK;
}

static esp_err_t panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end,
                                   int y_end, const void *color_data)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start must precede end");

    x_start += p->x_gap;
    x_end += p->x_gap;
    y_start += p->y_gap;
    y_end += p->y_gap;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, LCD_CMD_CASET,
                                                  (uint8_t[]){
                                                      (x_start >> 8) & 0xFF, x_start & 0xFF,
                                                      ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF},
                                                  4),
                        TAG, "caset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, LCD_CMD_RASET,
                                                  (uint8_t[]){
                                                      (y_start >> 8) & 0xFF, y_start & 0xFF,
                                                      ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF},
                                                  4),
                        TAG, "raset");

    size_t len = (size_t)(x_end - x_start) * (y_end - y_start) * p->fb_bits_per_pixel / 8;
    return esp_lcd_panel_io_tx_color(p->io, LCD_CMD_RAMWR, color_data, len);
}

static esp_err_t panel_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);
    return esp_lcd_panel_io_tx_param(p->io, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);

    if (mirror_x) {
        p->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        p->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    return esp_lcd_panel_io_tx_param(p->io, LCD_CMD_MADCTL, (uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);

    if (swap_axes) {
        p->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        p->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    return esp_lcd_panel_io_tx_param(p->io, LCD_CMD_MADCTL, (uint8_t[]){p->madctl_val}, 1);
}

static esp_err_t panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);
    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_disp_on_off(esp_lcd_panel_t *panel, bool on)
{
    jd9853_panel_t *p = __containerof(panel, jd9853_panel_t, base);
    return esp_lcd_panel_io_tx_param(p->io, on ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

esp_err_t board_lcd_new_panel_jd9853(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *cfg,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && cfg && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    jd9853_panel_t *p = calloc(1, sizeof(*p));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no mem for panel");

    esp_err_t ret = ESP_OK;
    if (cfg->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << cfg->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "reset gpio");
    }

    switch (cfg->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        p->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        p->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported element order");
    }

    switch (cfg->bits_per_pixel) {
    case 16:
        p->colmod_val = 0x55;
        p->fb_bits_per_pixel = 16;
        break;
    case 18:
        p->colmod_val = 0x66;
        /* RGB666 sends each component in the high 6 bits of its own byte. */
        p->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
    }

    p->io = io;
    p->reset_gpio_num = cfg->reset_gpio_num;
    p->reset_level = cfg->flags.reset_active_high;
    p->base.del = panel_del;
    p->base.reset = panel_reset;
    p->base.init = panel_init;
    p->base.draw_bitmap = panel_draw_bitmap;
    p->base.invert_color = panel_invert_color;
    p->base.set_gap = panel_set_gap;
    p->base.mirror = panel_mirror;
    p->base.swap_xy = panel_swap_xy;
    p->base.disp_on_off = panel_disp_on_off;

    *ret_panel = &p->base;
    return ESP_OK;

err:
    if (cfg->reset_gpio_num >= 0) {
        gpio_reset_pin(cfg->reset_gpio_num);
    }
    free(p);
    return ret;
}
