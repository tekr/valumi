/*
 * valumi - live OKX prices on a 320x172 landscape dashboard.
 *
 * Tasks:
 *   main (this file)  the screen, the button and the touch panel: renders at
 *                     ~12 Hz idle and flat out while animating, and applies
 *                     display settings the moment they change
 *   net (market.c)    prices and charts from the exchange
 *   httpd             the web panel (web_panel.c)
 *   wifi_mgr          networks and the setup hotspot
 * They share state only through the settings store, the carousel and the
 * market's snapshot.
 *
 * Runs on both 1.47" C6 boards. On the Touch variant a swipe changes coin, a
 * resting finger holds the current one, and the accelerometer keeps the page
 * upright through either landscape orientation. On the plain board those
 * inputs never fire and the orientation is whatever the panel says.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_config.h"
#include "auth.h"
#include "board_button.h"
#include "board_display.h"
#include "board_gfx.h"
#include "board_orientation.h"
#include "board_touch.h"
#include "board_variant.h"
#include "button_seq.h"
#include "carousel.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "firmware.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "local_time.h"
#include "market.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "page_transition.h"
#include "screens.h"
#include "settings_store.h"
#include "ui.h"
#include "web_panel.h"
#include "wifi_mgr.h"

static const char *TAG = "valumi";

/* Limits that live in more than one module, tied together here so changing
 * one without the other fails the build rather than the device. */
_Static_assert(SET_MAX_NETS == WIFI_MGR_MAX_NETS, "settings and Wi-Fi disagree on network count");
_Static_assert(SET_NUM_RANGES == APP_NUM_RANGES, "settings and chart ranges disagree");
_Static_assert(APP_BRIGHTNESS_DEFAULT >= SET_BRIGHT_MIN && APP_BRIGHTNESS_DEFAULT <= SET_BRIGHT_MAX,
               "default brightness outside the settable range");
_Static_assert(sizeof(((ui_coin_t *)0)->label) > SET_LABEL_MAX, "ui label buffer too small");
_Static_assert(SET_CHART_CLOSE == UI_CHART_CLOSE && SET_CHART_HIGHS_LOWS == UI_CHART_HIGHS_LOWS &&
                   SET_CHART_BAND == UI_CHART_BAND,
               "settings and ui disagree on chart styles");
_Static_assert(sizeof(((wifi_mgr_net_t *)0)->ssid) > SET_SSID_MAX, "SSID buffer too small");
_Static_assert(sizeof(((wifi_mgr_net_t *)0)->password) > SET_WIFI_PASS_MAX,
               "Wi-Fi password buffer too small");

/* The render task's copy of the settings, refreshed when the generation
 * moves. Without the networks: it has no use for their passwords. */
static settings_t s_cfg;
static uint32_t s_cfg_gen;

static ui_coin_t s_snap[SET_MAX_COINS];
static int64_t s_overlay_until_us; /* brightness overlay */

/* Tick flash: when a poll changes a price, flash for 300 ms. */
static float s_prev_price[SET_MAX_COINS];
static int64_t s_flash_start_us[SET_MAX_COINS];
#define FLASH_DURATION_US 300000

static float ease_in_out(float t)
{
    return t * t * (3.0f - 2.0f * t); /* smoothstep */
}

/* ---- Page transition ---------------------------------------------------- */

typedef struct {
    bool active;
    int from, to;   /* page indices */
    int dir;        /* +1: the new page enters from the right or below, -1: the other way */
    int64_t t0_us;
} transition_t;

static transition_t s_tr;
static int s_shown_page;
/* Direction of the page change waiting for the current transition to finish.
 * Without it, a swipe that lands mid-transition would animate in the default
 * direction and appear to move the wrong way. */
static int s_pending_dir;

/* One frame of the page change. Returns false when it has finished. */
static bool render_transition(uint16_t *fb, const ui_coin_t *coins)
{
    float p = (float)(esp_timer_get_time() - s_tr.t0_us) / (s_cfg.transition_ms * 1000.0f);
    if (p >= 1.0f) {
        return false;
    }
    const page_transition_t t = {.move = s_cfg.transition_move,
                                 .vertical = s_cfg.transition_vertical,
                                 .style = s_cfg.transition_style,
                                 .fade = s_cfg.transition_fade};
    page_transition_render(fb, &t, &coins[s_tr.from], &coins[s_tr.to], p, s_tr.dir);
    return true;
}

/* ---- Orientation spin --------------------------------------------------- */

/* Turning the board over turns the picture with it. The panel can flip for
 * free -- two bits of MADCTL -- but snapping is jarring, so the frame is
 * rotated through 180 degrees first and the flip applied at the end, where a
 * half turn of content and a flipped panel are the same pixels.
 *
 * That needs a copy of the frame to rotate out of. A full one (110 KB) never
 * fits beside Wi-Fi and TLS; half size, 27.5 KB, does, and costs almost
 * nothing visible: the fit scale is below 62% for three quarters of the turn.
 * If even that fails, crossfade, which needs no buffer at all. */
#define SPIN_SRC_W (BOARD_LCD_V_RES / 2)
#define SPIN_SRC_H (BOARD_LCD_H_RES / 2)
#define SPIN_SRC_BYTES (SPIN_SRC_W * SPIN_SRC_H * 2)
#define SPIN_DURATION_US 560000

typedef struct {
    bool active;
    int64_t t0_us;
    uint16_t *src; /* half-size frame capture, or NULL to crossfade */
    bool target_flip;
    bool applied; /* the panel flip has been pushed (crossfade path) */
} spin_t;

static spin_t s_spin;

/* Begin a turn away from @p current, which must be the frame just presented. */
static void spin_start(bool target_flip, const uint16_t *current)
{
    s_spin = (spin_t){.active = true, .t0_us = esp_timer_get_time(), .target_flip = target_flip};
    /* Only the CPU reads it, so it need not be DMA-capable -- part of what
     * makes it fit at all with TLS up. */
    s_spin.src = heap_caps_malloc(SPIN_SRC_BYTES, MALLOC_CAP_INTERNAL);
    if (s_spin.src) {
        board_gfx_shrink_half(s_spin.src, current);
    }
}

static void spin_finish(void)
{
    board_display_set_flipped(s_spin.target_flip);
    free(s_spin.src);
    s_spin = (spin_t){0};
}

/* One frame of the turn. Returns false when it is over. */
static bool render_spin(uint16_t *fb, const ui_coin_t *coin)
{
    float p = (float)(esp_timer_get_time() - s_spin.t0_us) / SPIN_DURATION_US;
    if (p >= 1.0f) {
        /* The last frame was a half turn, which is exactly what the flipped
         * panel shows of an unrotated one: the flip is invisible here. */
        spin_finish();
        return false;
    }
    if (s_spin.src) {
        board_gfx_rotate_blit(fb, s_spin.src, SPIN_SRC_W, SPIN_SRC_H,
                              ease_in_out(p) * 3.14159265f, board_rgb565(0, 0, 0));
        return true;
    }
    /* Crossfade: out, flip at the midpoint, back in. Redrawn rather than
     * replayed, so a price that ticks meanwhile is simply current. */
    if (p >= 0.5f && !s_spin.applied) {
        board_display_set_flipped(s_spin.target_flip);
        s_spin.applied = true;
    }
    float a = p < 0.5f ? 1.0f - p * 2.0f : p * 2.0f - 1.0f;
    ui_begin_frame(fb);
    ui_draw_page(fb, coin, (uint8_t)(a * 255.0f));
    return true;
}

/* A screen now covers the ticker: land anything in motion, so nothing waits
 * half-done underneath it -- a turn would otherwise hold its buffer and block
 * every flip until the screen went. */
static void finish_animations(void)
{
    if (s_tr.active) {
        s_shown_page = s_tr.to;
        s_tr.active = false;
    }
    if (s_spin.active) {
        spin_finish();
    }
}

/* ---- Settings and the button -------------------------------------------- */

/* The night level inside the night window, once the clock is set; the day
 * level otherwise. */
static bool night_now(void)
{
    if (!s_cfg.night_on || !market_clock_synced()) {
        return false;
    }
    int minute = local_minute_of_day((int64_t)time(NULL), s_cfg.tz_offset_min);
    return local_in_window(minute, s_cfg.night_start_min, s_cfg.night_end_min);
}

static int s_applied_brightness = -1;

static void apply_brightness(void)
{
    int want = night_now() ? s_cfg.night_brightness : s_cfg.brightness;
    if (want != s_applied_brightness) {
        board_display_set_brightness((uint8_t)want);
        s_applied_brightness = want;
    }
}

static bool wanted_flip(void)
{
    if (s_cfg.orient_fixed || !board_orientation_available()) {
        return s_cfg.usb_left;
    }
    return board_orientation_flipped();
}

static void refresh_settings(void)
{
    uint32_t g = settings_generation();
    if (g == s_cfg_gen) {
        return;
    }
    s_cfg_gen = g;
    settings_get(&s_cfg);
    memset(s_cfg.nets, 0, sizeof(s_cfg.nets));
    apply_brightness();
}

/* Short press: step whichever brightness is in force, and keep it. */
static void step_brightness(void)
{
    settings_t *s = settings_dup();
    if (s == NULL) {
        return;
    }
    int *level = night_now() ? &s->night_brightness : &s->brightness;
    *level += APP_BRIGHTNESS_STEP;
    if (*level > SET_BRIGHT_MAX) {
        *level = SET_BRIGHT_MIN;
    }
    settings_update(s, SETTINGS_SAVE_LATER);
    settings_free(s);
    refresh_settings();
    s_overlay_until_us = esp_timer_get_time() + 1500000;
}

static void step_range(void)
{
    settings_t *s = settings_dup();
    if (s == NULL) {
        return;
    }
    s->range = (s->range + 1) % APP_NUM_RANGES;
    settings_update(s, SETTINGS_SAVE_LATER);
    settings_free(s);
    refresh_settings();
}

/* ---- Screens over the ticker -------------------------------------------- */

static screen_t s_screen;
static ui_qr_t s_qr; /* encoded once per screen, not per frame */
static int64_t s_qr_screen_until_us; /* address / login screen; 0 = not showing */
/* The setup screen changes once a phone has joined the hotspot: join code
 * first, open-the-panel code after. -1: not built. */
static int s_setup_shown = -1;

static void encode_qr(void)
{
    if (s_screen.qr_text[0] == '\0' || !ui_qr_make(&s_qr, s_screen.qr_text)) {
        s_qr.size = 0;
    }
}

/* The address / login screen. Online, its code is a one-time login link. */
static void show_login_screen(const char *title, int seconds)
{
    char token[AUTH_TOKEN_HEX + 1] = "";
    uint32_t ip = wifi_mgr_ip();
    if (ip != 0) {
        auth_issue_qr(seconds, token);
    }
    screens_login(&s_screen, title, ip, token, wifi_mgr_ssid(), s_cfg.panel_pw);
    encode_qr();
    memset(token, 0, sizeof(token));
    s_qr_screen_until_us = esp_timer_get_time() + seconds * 1000000LL;
    s_setup_shown = -1;
}

static void build_setup_screen(bool has_clients)
{
    screens_setup(&s_screen, has_clients, wifi_mgr_ap_ssid(), wifi_mgr_ap_password());
    encode_qr();
    s_setup_shown = has_clients;
}

static void draw_qr_screen(uint16_t *fb)
{
    const char *lines[SCREEN_MAX_LINES];
    for (int i = 0; i < s_screen.n_lines; i++) {
        lines[i] = s_screen.lines[i];
    }
    ui_render_qr_screen(fb, s_qr.size ? &s_qr : NULL, s_screen.title, lines, s_screen.n_lines);
}

/* ---- Rendering the ticker ----------------------------------------------- */

/* The market's coins, plus the tick flash. Returns the coin count; a new
 * list (*gen changed) starts with no flash state. */
static int snapshot_coins(uint32_t *gen)
{
    int64_t now = esp_timer_get_time();
    uint32_t before = *gen;
    int n = market_snapshot(s_snap, s_cfg.range, gen);
    if (*gen != before) {
        memset(s_prev_price, 0, sizeof(s_prev_price));
        memset(s_flash_start_us, 0, sizeof(s_flash_start_us));
    }
    for (int i = 0; i < n; i++) {
        ui_coin_t *o = &s_snap[i];
        o->chart = (ui_chart_t)s_cfg.chart_style;
        /* Not on the first sample, so boot does not open with a flash. */
        if (o->last > 0.0f && s_prev_price[i] > 0.0f && o->last != s_prev_price[i]) {
            s_flash_start_us[i] = now;
        }
        if (o->last > 0.0f) {
            s_prev_price[i] = o->last;
        }
        float elapsed = (float)(now - s_flash_start_us[i]);
        if (s_flash_start_us[i] != 0 && elapsed < FLASH_DURATION_US) {
            o->flash = 1.0f - ease_in_out(elapsed / FLASH_DURATION_US);
        }
    }
    return n;
}

static void draw_settled_page(uint16_t *fb)
{
    ui_begin_frame(fb);
    ui_draw_page(fb, &s_snap[s_shown_page], 255);
}

static void render_ticker(uint16_t *fb, int n, ui_net_status_t net)
{
    if (s_spin.active) {
        /* The turn owns the whole frame: the status dot and any overlay were
         * captured with it and rotate along. Its last call only applies the
         * flip, so the page is redrawn here rather than showing one soft,
         * half-resolution frame through the new orientation. */
        if (!render_spin(fb, &s_snap[s_shown_page])) {
            draw_settled_page(fb);
            ui_draw_status(fb, net, 1.0f);
        }
        return;
    }
    int page = carousel_page();
    if (page >= n) {
        page = 0;
    }
    /* Compare against the transition's DESTINATION, never the page on
     * screen: s_shown_page holds the outgoing page for the whole transition,
     * so testing it here cancelled every transition on its second frame.
     * Only a target that differs from where we are heading is new. */
    if (s_tr.active && page != s_tr.to) {
        /* A swipe landed mid-transition: snap to its destination and start
         * afresh, so the new one runs its full length the right way. */
        s_shown_page = s_tr.to;
        s_tr.active = false;
    }
    if (!s_tr.active && page != s_shown_page) {
        s_tr = (transition_t){
            .active = true,
            .from = s_shown_page,
            .to = page,
            .dir = s_pending_dir != 0 ? s_pending_dir : 1,
            .t0_us = esp_timer_get_time(),
        };
        s_pending_dir = 0;
    }
    if (!s_tr.active) {
        draw_settled_page(fb);
    } else if (!render_transition(fb, s_snap)) {
        s_shown_page = s_tr.to;
        s_tr.active = false;
        draw_settled_page(fb);
    }
    /* ~2.5 s sine breathe; only the disconnected state uses it. */
    float pulse = 0.5f + 0.5f * sinf((float)(esp_timer_get_time() % 2500000) * 2.513e-6f);
    ui_draw_status(fb, net, pulse);
    if (esp_timer_get_time() < s_overlay_until_us) {
        ui_render_brightness_overlay(fb, s_applied_brightness);
    }
}

/* ---- Start-up ----------------------------------------------------------- */

static void start_wifi(void)
{
    settings_t *full = settings_dup();
    wifi_mgr_net_t *nets = calloc(SET_MAX_NETS, sizeof(*nets));
    assert(full && nets);
    for (int i = 0; i < full->n_nets; i++) {
        const set_net_t *n = &full->nets[i];
        strlcpy(nets[i].ssid, n->ssid, sizeof(nets[i].ssid));
        strlcpy(nets[i].password, n->password, sizeof(nets[i].password));
        nets[i].static_ip = n->static_ip;
        nets[i].ip = n->ip;
        nets[i].mask = n->mask;
        nets[i].gw = n->gw;
        nets[i].dns = n->dns;
    }
    ESP_ERROR_CHECK(wifi_mgr_start(nets, full->n_nets));
    memset(nets, 0, SET_MAX_NETS * sizeof(*nets));
    free(nets);
    settings_free(full);
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable");
        return;
    }
    mdns_hostname_set(APP_HOSTNAME);
    mdns_instance_name_set("valumi");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    /* The QR encoder logs the text it encodes at INFO: the one-time login
     * link and the hotspot passphrase. */
    esp_log_level_set("QRCODE", ESP_LOG_WARN);

    board_display_cfg_t disp_cfg = {
        .landscape = true,
        .single_buffer = true,
    };
    ESP_ERROR_CHECK(board_display_init_ex(&disp_cfg));
    ESP_ERROR_CHECK(board_button_init());
    settings_store_init();
    refresh_settings(); /* and with it the brightness */

    /* Both optional, and both absent on the plain board: every caller below
     * copes with "not there", so the results only decide what is logged. */
    ESP_LOGI(TAG, "board: %s", board_variant_name());
    if (board_touch_init() == ESP_OK) {
        ESP_LOGI(TAG, "touch: swipe to change coin, hold to pause");
    }
    if (board_orientation_init() == ESP_OK) {
        ESP_LOGI(TAG, "orientation: display follows the accelerometer");
    }
    board_display_set_flipped(wanted_flip());
    ui_render_splash(board_display_back_buffer(), "CONNECTING");
    board_display_present();

    start_wifi();
    start_mdns();

    /* Single core: rendering must outrank our own networking or every poll
     * burst freezes mid-transition. Wi-Fi and lwIP sit far above both (18+),
     * so this starves only our TLS and JSON work, which is what should
     * yield. */
    vTaskPrioritySet(NULL, 10);
    carousel_init(s_cfg.n_coins, s_cfg.dwell_s);
    market_start();
    ESP_ERROR_CHECK(web_panel_start());

    bool have_any_data = false;
    uint32_t seen_coins_gen = 0;
    uint32_t seen_join = 0;
    bool been_in_setup = false;
    bool reset_pending = false;
    TickType_t last_wake = xTaskGetTickCount();
    bseq_t button;
    bseq_init(&button);
    int64_t last_night_check_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        refresh_settings();
        if (now - last_night_check_us > 1000000) {
            last_night_check_us = now;
            apply_brightness(); /* the night window opens and closes on its own */
        }
        firmware_confirm_when_settled(wifi_mgr_state() != WIFI_MGR_CONNECTING, now);

        bool setup = wifi_mgr_state() == WIFI_MGR_SETUP;
        switch (bseq_update(&button, board_button_is_down(), now)) {
        case BSEQ_SHORT:
            step_brightness();
            break;
        case BSEQ_LONG:
            /* Fires while still held, so the chart answers the hold itself. */
            step_range();
            break;
        case BSEQ_LOGIN:
            if (!setup) {
                show_login_screen("LOG IN", APP_LOGIN_SCREEN_S);
            }
            break;
        case BSEQ_RESET:
            reset_pending = true;
            break;
        default:
            break;
        }

        /* The address shows on the first connection after power-on and on the
         * way out of setup mode -- the two moments someone is looking for it.
         * Not on every rejoin: a flaky network would otherwise keep covering
         * the prices, and replace a code someone is in the middle of
         * scanning. */
        been_in_setup = been_in_setup || setup;
        uint32_t join = wifi_mgr_join_count();
        if (join != seen_join) {
            bool first = seen_join == 0;
            seen_join = join;
            if ((first || been_in_setup) && now >= s_qr_screen_until_us) {
                show_login_screen("CONNECTED", APP_ADDRESS_SCREEN_S);
            }
            been_in_setup = false;
        }

        bool qr_screen = now < s_qr_screen_until_us;
        if (!setup) {
            s_setup_shown = -1;
        } else if (!qr_screen && s_setup_shown != (wifi_mgr_ap_clients() > 0)) {
            build_setup_screen(wifi_mgr_ap_clients() > 0);
        }
        bool covered = reset_pending || qr_screen || setup;

        /* All zeros on the plain board, so every branch below is inert. */
        board_touch_state_t touch = {0};
        board_touch_poll(&touch);
        int moved;
        if (!covered && touch.gesture == BOARD_TOUCH_SWIPE_LEFT) {
            /* The content follows the finger: swiping left brings the next
             * page in, the same direction the timer moves. */
            moved = carousel_jump(1);
        } else if (!covered && touch.gesture == BOARD_TOUCH_SWIPE_RIGHT) {
            moved = carousel_jump(-1);
        } else {
            /* A finger on the glass holds the page, and so does a screen
             * over the ticker or anything still moving: a page's time starts
             * once it has arrived, so at 0 s the coins scroll continuously. */
            moved = carousel_tick(touch.down || s_tr.active || s_spin.active || covered);
        }
        if (moved != 0) {
            s_pending_dir = moved;
        }

        uint32_t gen = seen_coins_gen;
        int n = snapshot_coins(&gen);
        if (gen != seen_coins_gen) {
            /* Indices name different coins now. */
            seen_coins_gen = gen;
            s_tr.active = false;
            s_pending_dir = 0;
            s_shown_page = carousel_page();
        }
        if (!have_any_data) {
            for (int i = 0; i < n && !have_any_data; i++) {
                have_any_data = s_snap[i].last > 0.0f;
            }
            /* Land on the current page rather than transitioning out of the
             * splash. */
            s_shown_page = carousel_page();
        }
        if (s_shown_page >= n) {
            s_shown_page = 0;
        }

        ui_net_status_t net = !wifi_mgr_connected() ? UI_NET_CONNECTING
                              : market_fetch_ok()   ? UI_NET_OK
                                                    : UI_NET_DEGRADED;
        uint16_t *fb = board_display_back_buffer();
        bool ticker_shown = !covered && have_any_data;
        if (!ticker_shown) {
            finish_animations();
        }
        if (reset_pending) {
            ui_render_splash(fb, "FACTORY RESET - RESTARTING");
        } else if (covered) {
            draw_qr_screen(fb);
        } else if (!have_any_data) {
            char line[56];
            if (!wifi_mgr_connected()) {
                /* Names the network being tried, so a sweep across several is
                 * visible rather than looking like one stuck attempt. */
                snprintf(line, sizeof(line), "WIFI: CONNECTING TO %s", wifi_mgr_ssid());
            } else {
                market_progress(line, sizeof(line));
            }
            ui_render_splash(fb, line);
        } else {
            render_ticker(fb, n, net);
        }

        /* The hold countdown goes over whatever is showing. */
        int secs = 0;
        bseq_hint_t hint = bseq_hint(&button, now, &secs);
        if (hint != BSEQ_HINT_NONE && !reset_pending && !s_spin.active) {
            if (setup) {
                /* Releasing does nothing in setup mode -- its screen already
                 * shows everything the login screen would. */
                bseq_reset_countdown(&button, now, &secs);
                ui_render_hold_overlay(fb, UI_HOLD_RESET_ONLY, secs);
            } else {
                ui_render_hold_overlay(
                    fb, hint == BSEQ_HINT_LOGIN ? UI_HOLD_LOGIN : UI_HOLD_RESET, secs);
            }
        }

        board_display_present();

        if (reset_pending) {
            /* Or a reset during probation would roll the firmware back too. */
            firmware_confirm();
            settings_factory_reset();
        }

        /* Start a turn only from a settled frame of the ticker, after
         * present(), so the captured frame is exactly what is on the glass.
         * Any other screen simply flips. */
        if (!s_spin.active && !s_tr.active) {
            bool want = wanted_flip();
            if (want != board_display_flipped()) {
                if (ticker_shown && hint == BSEQ_HINT_NONE) {
                    spin_start(want, fb);
                } else {
                    board_display_set_flipped(want);
                }
            }
        }

        /* While animating, run unthrottled: progress is computed from absolute
         * time, and quantising frames to a fixed grid alternates "late" and
         * "waiting", which is judder. A finger on the glass counts too: the
         * idle 12.5 Hz would sample a 300 ms swipe only four times. */
        bool animating = s_tr.active || s_spin.active || touch.down ||
                         (n > 0 && s_snap[s_shown_page].flash > 0.0f);
        if (animating) {
            vTaskDelay(1);
            last_wake = xTaskGetTickCount();
        } else {
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(80));
        }
    }
}
