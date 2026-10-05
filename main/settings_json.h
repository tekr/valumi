/*
 * The JSON the web panel speaks, to and from settings_t.
 *
 * Passwords only travel inward: a network reports "has_password", and the
 * panel sends "keep_password" to leave a stored one alone.
 *
 * Every apply is all-or-nothing: the change is made on a copy, validated
 * whole, and only then written back.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What an apply changed. */
#define SET_CHG_ANY 0x01
#define SET_CHG_NETS 0x02  /* networks: they take effect after a restart */
#define SET_CHG_COINS 0x04 /* coins added or re-ordered: see settings_new_coins() */

/** Everything but passwords. Caller owns the result. */
cJSON *settings_to_json(const settings_t *s);

/**
 * @brief Apply whichever live fields @p j contains.
 *
 * Unknown keys are rejected rather than ignored, so a typo cannot look like
 * success. Networks are refused: they go through settings_apply_wifi_json().
 *
 * @return SET_CHG_* flags (0 = nothing changed), or -1 with @p err set.
 */
int settings_apply_json(settings_t *s, const cJSON *j, char *err, size_t en);

/**
 * @brief Replace the network list from {"networks": [...]}.
 *
 * Each entry: {ssid, password | keep_password, static_ip, ip, mask, gateway,
 * dns}. keep_password takes the password stored for that SSID, so the panel
 * can reorder or re-address networks without ever having seen one.
 */
int settings_apply_wifi_json(settings_t *s, const cJSON *req, char *err, size_t en);

/**
 * @brief Restore an export: live fields and networks, atomically.
 *
 * A network listed without a password takes the one this device already has
 * for that SSID; one it has none for is skipped and described in @p warnings
 * (may be NULL). If none can be imported, the device's own stay.
 */
int settings_import_json(settings_t *s, const cJSON *j, cJSON *warnings, char *err, size_t en);

/** A backup: everything, Wi-Fi passwords included -- the file is as
 * sensitive as they are. Not the panel password, which belongs to each
 * ticker. Caller owns the result. */
cJSON *settings_to_export_json(const settings_t *s);

/** The settings as saved on the device: the export plus the panel password.
 * Caller owns the result. */
cJSON *settings_to_stored_json(const settings_t *s);

/**
 * @brief Read saved settings onto @p s, field by field.
 *
 * A field that is missing, unknown or invalid keeps its value in @p s, so
 * settings saved by older or newer firmware load as far as they still make
 * sense. @p s must already hold a valid panel password.
 */
void settings_from_stored_json(settings_t *s, const cJSON *j);

/**
 * @brief Cheap bounds to check before cJSON_Parse on anything from the
 * network: its parser recurses per nesting level and allocates per element,
 * so either could exhaust the server task's stack or the heap.
 */
bool settings_json_bounded(const char *text, size_t len, int max_depth, int max_elements);

/** Indices into @p after of coins that were not in @p before. */
int settings_new_coins(const settings_t *before, const settings_t *after, int *idx, int max);

bool settings_parse_hhmm(const char *str, int *out_min);
void settings_format_hhmm(int min, char *out, size_t n);

#ifdef __cplusplus
}
#endif
