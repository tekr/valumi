/*
 * The web control panel: one embedded page and a small JSON API.
 *
 * Plain HTTP on port 80. A TLS server would need a second ~40 KB TLS context
 * next to the OKX session, and the heap does not have it; the panel password
 * therefore crosses the local network in the clear, which is an accepted risk
 * for a desk ticker on a home network or the owner's own hotspot.
 *
 * Access:
 *   setup mode   open -- the hotspot's WPA2 passphrase, shown only on the
 *                ticker's screen, is the gate
 *   otherwise    a session: from the panel password, or from the one-time
 *                QR code on the ticker's screen
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_panel_start(void);

#ifdef __cplusplus
}
#endif
