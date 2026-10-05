#include "screens.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "wifi_mgr.h"
#include "wifi_qr.h"

static void line(screen_t *s, const char *fmt, ...)
{
    if (s->n_lines >= SCREEN_MAX_LINES) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->lines[s->n_lines], SCREEN_LINE_MAX, fmt, ap);
    va_end(ap);
    s->n_lines++;
}

static void reset(screen_t *s, const char *title)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->title, sizeof(s->title), "%s", title);
}

void screens_login(screen_t *out, const char *title, uint32_t ip, const char *token,
                   const char *ssid, const char *panel_pw)
{
    reset(out, title);
    if (ip == 0) {
        line(out, "NOT CONNECTED YET");
        line(out, " %s", ssid);
    } else {
        char addr[16];
        snprintf(addr, sizeof(addr), "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255),
                 (unsigned)(ip >> 8 & 255), (unsigned)(ip & 255));
        snprintf(out->qr_text, sizeof(out->qr_text), "http://%s/login?t=%s", addr, token);
        line(out, "SCAN TO LOG IN, OR OPEN");
        line(out, " " APP_HOSTNAME ".LOCAL");
        line(out, "*%s", addr);
    }
    /* Always: without it, someone holding the ticker but without a camera
     * has no way in. */
    line(out, "PASSWORD");
    line(out, "*%s", panel_pw);
}

void screens_setup(screen_t *out, bool has_clients, const char *ap_ssid, const char *ap_password)
{
    reset(out, "SETUP");
    if (!has_clients) {
        if (!wifi_qr_join_text(ap_ssid, ap_password, out->qr_text, sizeof(out->qr_text))) {
            out->qr_text[0] = '\0';
        }
        line(out, "SCAN TO JOIN WI-FI");
        line(out, " %s", ap_ssid);
        line(out, "PASSWORD");
        line(out, "*%s", ap_password);
        /* The address too, as asked: a laptop joining by hand will not be
         * taken to the page by the captive portal the way a phone is. */
        line(out, "THEN OPEN " WIFI_MGR_AP_ADDR);
    } else {
        snprintf(out->qr_text, sizeof(out->qr_text), "%s", WIFI_MGR_AP_URL);
        line(out, "CONNECTED. NOW OPEN");
        line(out, "*" WIFI_MGR_AP_ADDR);
        line(out, "");
        line(out, "OR SCAN THE CODE");
    }
}
