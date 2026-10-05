/*
 * The BOOT button, usable as a normal input after reset. Raw level only:
 * debouncing and press timing belong to the caller (see valumi's
 * button_seq).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_button_init(void);

/** Raw current level: true while the button is held down. */
bool board_button_is_down(void);

#ifdef __cplusplus
}
#endif
