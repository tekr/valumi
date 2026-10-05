/*
 * The one live copy of the settings, kept in NVS.
 *
 * Readers take a copy (settings_get) and compare settings_generation() each
 * frame or loop to notice a change; there are no callbacks, so no task ever
 * runs another task's code. Writers go through settings_update(), which
 * validates, bumps the generation and persists.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load from NVS, or start from the defaults.
 *
 * Whenever there are no usable settings -- a blank device, or unreadable
 * ones -- seeds networks and the panel password from main/wifi_secrets.h if
 * that file exists (development builds).
 * The first boot after a factory reset does not: see settings_factory_reset().
 *
 * Needs nvs_flash_init() done first.
 */
esp_err_t settings_store_init(void);

/** Copy out the current settings. */
void settings_get(settings_t *out);

/**
 * @brief A heap copy of the settings, or NULL if out of memory.
 *
 * The copy holds every password; release it with settings_free(), which
 * wipes it. Used wherever a task needs a whole settings_t for a moment and
 * ~900 bytes is too much for its stack.
 */
settings_t *settings_dup(void);

/** Wipe and free a settings_dup() copy. NULL is fine. */
void settings_free(settings_t *s);

/** Write out a pending SETTINGS_SAVE_LATER now. Call before restarting. */
void settings_flush(void);

/** Changes whenever the settings do. Cheap; poll it. */
uint32_t settings_generation(void);

typedef enum {
    SETTINGS_SAVE_NOW,   /* persist before returning (panel changes) */
    SETTINGS_SAVE_LATER, /* persist a few seconds after the LAST such change
                          * (button presses: stepping brightness five times
                          * writes flash once, not five times) */
} settings_save_t;

/**
 * @brief Replace the settings with @p s.
 * @return ESP_ERR_INVALID_ARG if @p s does not validate (nothing changes).
 */
esp_err_t settings_update(const settings_t *s, settings_save_t when);

/**
 * @brief Erase every setting and reboot into setup mode. Does not return.
 *
 * Wi-Fi credentials the driver cached in its own NVS namespace are erased
 * too, so nothing of the old networks survives.
 */
void settings_factory_reset(void);

#ifdef __cplusplus
}
#endif
