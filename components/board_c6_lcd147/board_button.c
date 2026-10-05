#include "board_button.h"
#include "board_pins.h"
#include "board_variant.h"

#include "driver/gpio.h"

static gpio_num_t s_pin = GPIO_NUM_NC;

esp_err_t board_button_init(void)
{
    s_pin = board_pins()->button;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << s_pin,
        .mode = GPIO_MODE_INPUT,
        /* The board has its own pull-up; the internal one is harmless belt
         * and braces. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool board_button_is_down(void)
{
    if (s_pin == GPIO_NUM_NC) {
        return false; /* init not called: report "not pressed", never garbage */
    }
    return gpio_get_level(s_pin) == 0; /* active low */
}
