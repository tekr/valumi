/*
 * What the setup, address and login screens say, and what their QR codes
 * encode. Pure string assembly, tested on the host; main.c decides when a
 * screen shows and ui.c draws it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_MAX_LINES 5
#define SCREEN_LINE_MAX 48

typedef struct {
    char title[24];
    /* Lines starting with '*' are values to type in (drawn large), ' ' other
     * values (bright), the rest labels; an empty line is a small gap. See
     * ui_render_qr_screen(). */
    char lines[SCREEN_MAX_LINES][SCREEN_LINE_MAX];
    int n_lines;
    char qr_text[128]; /* empty: no code */
} screen_t;

/**
 * @brief The address / login screen.
 *
 * @param ip      station address, host order; 0 = not connected, in which
 *                case there is no code and the screen says so
 * @param token   one-time login token for the code (ignored when ip is 0)
 * @param ssid    the network being tried, shown when not connected
 */
void screens_login(screen_t *out, const char *title, uint32_t ip, const char *token,
                   const char *ssid, const char *panel_pw);

/**
 * @brief Setup mode. Before a phone joins: a code that joins the hotspot
 * (password included) plus the details for typing it in. After: a code that
 * opens the panel.
 */
void screens_setup(screen_t *out, bool has_clients, const char *ap_ssid, const char *ap_password);

#ifdef __cplusplus
}
#endif
