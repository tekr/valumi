#include "wifi_mgr.h"
#include "app_config.h"
#include "captive_dns.h"
#include "code.h"
#include "wifi_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "dhcpserver/dhcpserver.h"
#include "lwip/ip4_addr.h"
#include "ping/ping_sock.h"

static const char *TAG = "wifi_mgr";

#define CONNECTED_BIT BIT0
#define FAILED_BIT BIT1

/* In setup mode the saved networks are retried less eagerly than usual. Each
 * sweep hops the radio across channels, and a phone joining the hotspot in
 * that moment sees a stutter; the longer pause makes that rare without making
 * recovery slow. */
#define SETUP_SWEEP_PAUSE_S 20
/* With someone on the hotspot, joining another network is off (it can move
 * the radio's channel, and the hotspot with it). But a phone that once joined
 * through the QR code may rejoin by itself during an outage and never leave,
 * which would keep the ticker in setup mode after the router is back. So it
 * still LOOKS this often -- an SSID-filtered scan, which returns to the
 * hotspot's channel between channels -- and only moves if a saved network is
 * actually there. */
#define SETUP_LOOK_WITH_CLIENTS_S 60

static EventGroupHandle_t s_events;
static volatile bool s_connected;
static volatile bool s_setup;
static volatile uint32_t s_ip;
static volatile uint32_t s_join_count;

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static wifi_mgr_net_t s_nets[WIFI_MGR_MAX_NETS];
static int s_count;
#define AP_PASS_LEN 8
static char s_ap_ssid[33];
static char s_ap_password[AP_PASS_LEN + 1];

/* Index into s_nets of the network we are on, or -1. Written only by the
 * manager task; read by anything that wants to display it. */
static volatile int s_current = -1;
/* The one being attempted right now, so the splash can name it. */
static volatile int s_attempting;
/* Set while we are the ones tearing a link down, so a deliberate disconnect
 * is not logged as a loss. */
static bool s_expect_disconnect;
/* Gap before the next look for a better network. Grows each time an upgrade
 * is offered and refused -- see the comment where it is applied. */
static int s_upgrade_gap_s;

/* One user of the radio at a time: a connection attempt, the manager's own
 * upgrade scan, or a scan the web panel asked for. The driver refuses a scan
 * while it is connecting, so without this a panel scan would fail at random. */
static SemaphoreHandle_t s_radio;

static wifi_mgr_scan_rec_t s_scan_cache[WIFI_MGR_SCAN_MAX];
static int s_scan_cache_n = -1;

static void apply_static_ip(const wifi_mgr_net_t *n)
{
    esp_netif_ip_info_t info = {0};
    info.ip.addr = htonl(n->ip);
    info.netmask.addr = htonl(n->mask);
    info.gw.addr = htonl(n->gw);
    esp_err_t err = esp_netif_set_ip_info(s_sta_netif, &info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static address for '%s' rejected: %s", n->ssid, esp_err_to_name(err));
        return;
    }
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = htonl(n->dns ? n->dns : n->gw);
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
}

/* The event handler does not reconnect on its own. It did once, and that
 * raced the manager task: the driver would be re-dialling the last SSID while
 * the task set up the next one. Its job is to report -- and, for a static
 * address, to apply it the moment the link is up, which is what makes the
 * stack announce the address exactly as DHCP would. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        int i = s_attempting;
        if (i >= 0 && i < s_count && s_nets[i].static_ip) {
            apply_static_ip(&s_nets[i]);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        bool was_connected = s_connected;
        s_connected = false;
        s_ip = 0;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        xEventGroupSetBits(s_events, FAILED_BIT);
        if (was_connected && !s_expect_disconnect) {
            ESP_LOGW(TAG, "lost '%s' (reason %d)", wifi_mgr_ssid(), d->reason);
        } else {
            ESP_LOGD(TAG, "'%s' did not accept us (reason %d)", wifi_mgr_ssid(), d->reason);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR " on '%s'", IP2STR(&e->ip_info.ip), wifi_mgr_ssid());
        s_ip = ntohl(e->ip_info.ip.addr);
        s_connected = true;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

/* ---- Gateway check for static addresses ---------------------------------- */

/* With DHCP, an address arriving proves the network works. A static address
 * is "up" the moment the link is, whether or not it is right -- a mistyped
 * subnet would leave the ticker reporting ONLINE, unreachable, and never
 * falling back to setup mode, where the mistake could be fixed. So a static
 * network must reach its gateway before it counts. */
static SemaphoreHandle_t s_ping_done;
static volatile int s_ping_replies;

static void ping_ok(esp_ping_handle_t h, void *arg)
{
    s_ping_replies++;
}

static void ping_end(esp_ping_handle_t h, void *arg)
{
    xSemaphoreGive(s_ping_done);
}

static bool gateway_answers(uint32_t gw)
{
    if (s_ping_done == NULL) {
        s_ping_done = xSemaphoreCreateBinary();
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = 3;
    cfg.interval_ms = 300;
    cfg.timeout_ms = 1000;
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = htonl(gw);
    esp_ping_callbacks_t cbs = {.on_ping_success = ping_ok, .on_ping_end = ping_end};
    esp_ping_handle_t h;
    s_ping_replies = 0;
    xSemaphoreTake(s_ping_done, 0);
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
        return true; /* cannot check: do not strand a network on our own failure */
    }
    esp_ping_start(h);
    xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(6000));
    esp_ping_stop(h);
    esp_ping_delete_session(h);
    return s_ping_replies > 0;
}

/* Try one network and wait for a verdict. Returns true once an IP has
 * arrived -- association alone is not enough, since a captive portal or a
 * DHCP-less AP would otherwise count as success. Caller holds s_radio. */
static bool try_connect(int i)
{
    s_attempting = i;

    /* Any previous association has to go before the config changes, or the
     * driver applies the new SSID to a link it is still holding. This is our
     * own doing, so do not let it be reported as a network dropping out. */
    s_expect_disconnect = true;
    esp_wifi_disconnect();
    s_expect_disconnect = false;

    /* DHCP or not is per network: a static address that is right at home is
     * wrong on the phone hotspot. The address itself goes on at association
     * (see on_wifi_event). */
    if (s_nets[i].static_ip) {
        esp_netif_dhcpc_stop(s_sta_netif);
    } else {
        /* Clear any static address left from an earlier network. The DHCP
         * client announces a lease only if it differs from what the
         * interface holds, so a lease equal to that stale address would
         * never be announced and the attempt would time out. */
        esp_netif_dhcpc_stop(s_sta_netif);
        esp_netif_ip_info_t zero = {0};
        esp_netif_set_ip_info(s_sta_netif, &zero);
        esp_netif_dhcpc_start(s_sta_netif);
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_nets[i].ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_nets[i].password, sizeof(cfg.sta.password));
    /* WPA2 minimum when there is a password: stops accidental association
     * with an open evil twin of a secured network. */
    cfg.sta.threshold.authmode = s_nets[i].password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config for '%s' rejected: %s", s_nets[i].ssid, esp_err_to_name(err));
        return false;
    }

    xEventGroupClearBits(s_events, CONNECTED_BIT | FAILED_BIT);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect to '%s' failed to start: %s", s_nets[i].ssid,
                 esp_err_to_name(err));
        return false;
    }

    /* Waiting on FAILED_BIT as well as the timeout matters: an AP that is out
     * of range answers in well under a second, and sitting out the full
     * timeout on each absent network would make a sweep take as long as the
     * list is long. */
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(APP_WIFI_ATTEMPT_TIMEOUT_S * 1000));
    if ((bits & CONNECTED_BIT) && (!s_nets[i].static_ip || gateway_answers(s_nets[i].gw))) {
        s_current = i;
        /* Counted here rather than on the IP event, so a static address that
         * turns out not to work never counts as a join. */
        s_join_count++;
        return true;
    }
    if (bits & CONNECTED_BIT) {
        ESP_LOGW(TAG, "'%s': no answer from gateway " IPSTR " at the static address - "
                      "treating the network as unavailable", s_nets[i].ssid,
                 IP2STR(&(esp_ip4_addr_t){.addr = htonl(s_nets[i].gw)}));
        s_connected = false;
        s_ip = 0;
    }

    /* Leave nothing in flight for the next attempt to trip over. */
    s_expect_disconnect = true;
    esp_wifi_disconnect();
    s_expect_disconnect = false;
    return false;
}

/* Is any network above @p current in range? Returns its index, or -1.
 *
 * A scan takes the radio off channel for about a second, so this only ever
 * runs while we are on a fallback network -- on the first choice there is
 * nothing better to find and the data path is left alone entirely. Caller
 * holds s_radio. */
static int scan_for_better(int current)
{
    for (int i = 0; i < current; i++) {
        wifi_scan_config_t cfg = {
            .ssid = (uint8_t *)s_nets[i].ssid,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active = {.min = 40, .max = 80},
            /* Harmless on a station alone; with a phone on the setup
             * hotspot it is what keeps the phone connected through it. */
            .home_chan_dwell_time = 60,
        };
        if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
            continue;
        }

        uint16_t found = 0;
        esp_wifi_scan_get_ap_num(&found);
        /* The driver holds the result list until it is read or dropped.
         * Nothing here wants the records, so drop them -- skipping this leaks
         * a little on every scan, which at one scan per 15 s adds up. */
        esp_wifi_clear_ap_list();

        if (found > 0) {
            return i;
        }
    }
    return -1;
}

/* Full scan into the cache. Caller holds s_radio. */
static int scan_all_locked(void)
{
    wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 60, .max = 120},
        /* Back to the hotspot's channel between channels, so a phone on it
         * stays associated through the scan. */
        .home_chan_dwell_time = 60,
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t n = WIFI_MGR_SCAN_MAX;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    esp_wifi_scan_get_ap_records(&n, recs);
    esp_wifi_clear_ap_list(); /* anything beyond WIFI_MGR_SCAN_MAX */

    /* One entry per name, keeping the strongest; the driver returns records
     * strongest first, so the first sighting of a name wins. */
    int out = 0;
    for (int i = 0; i < n && out < WIFI_MGR_SCAN_MAX; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int k = 0; k < out && !dup; k++) {
            dup = strcmp(s_scan_cache[k].ssid, ssid) == 0;
        }
        if (dup) {
            continue;
        }
        strlcpy(s_scan_cache[out].ssid, ssid, sizeof(s_scan_cache[out].ssid));
        s_scan_cache[out].rssi = recs[i].rssi;
        s_scan_cache[out].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        out++;
    }
    free(recs);
    s_scan_cache_n = out;
    return out;
}

static void enter_setup(void)
{
    code_random(s_ap_password, AP_PASS_LEN, esp_random);
    ESP_LOGW(TAG, "setup mode: hotspot '%s' at " WIFI_MGR_AP_URL, s_ap_ssid);
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, s_ap_password, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    ap.ap.pmf_cfg.required = false;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hotspot config rejected: %s", esp_err_to_name(err));
    }
    captive_dns_start(WIFI_MGR_AP_IP);
    s_setup = true;

    /* Scan now, while nobody is on the hotspot, so the panel's first page
     * already has a list to offer. */
    scan_all_locked();
}

static void leave_setup(void)
{
    ESP_LOGI(TAG, "leaving setup mode");
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_setup = false;
}

static int ap_clients(void)
{
    if (!s_setup) {
        return 0;
    }
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return 0;
    }
    return list.num;
}

/* Is any saved network in range? Returns its index, or -1. Caller holds
 * s_radio. */
static int scan_for_saved(void)
{
    return scan_for_better(s_count);
}

static void wifi_task(void *arg)
{
    int64_t last_check_us = 0;
    int64_t last_setup_look_us = 0;
    int64_t down_since_us = esp_timer_get_time();
    bool was_connected = false;

    for (;;) {
        int64_t now = esp_timer_get_time();
        if (was_connected && !s_connected) {
            down_since_us = now;
        }
        was_connected = s_connected;

        if (!s_connected) {
            s_current = -1;
            wifi_policy_in_t in = {
                .n_nets = s_count,
                .setup = s_setup,
                .hotspot_clients = ap_clients(),
                .now_us = now,
                .down_since_us = down_since_us,
                .last_look_us = last_setup_look_us,
                .fallback_us = (int64_t)APP_SETUP_FALLBACK_S * 1000000,
                .look_every_us = (int64_t)SETUP_LOOK_WITH_CLIENTS_S * 1000000,
            };
            wifi_action_t act = wifi_policy(&in);
            if (act == WIFI_ACT_ENTER_SETUP) {
                xSemaphoreTake(s_radio, portMAX_DELAY);
                enter_setup();
                xSemaphoreGive(s_radio);
                continue; /* and ask again, now in setup mode */
            }
            if (act == WIFI_ACT_WAIT) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (act == WIFI_ACT_LOOK) {
                last_setup_look_us = now;
                xSemaphoreTake(s_radio, portMAX_DELAY);
                int seen = scan_for_saved();
                bool joined = seen >= 0 && try_connect(seen);
                if (joined) {
                    leave_setup();
                }
                xSemaphoreGive(s_radio);
                if (joined) {
                    ESP_LOGI(TAG, "'%s' is back; leaving setup mode", s_nets[seen].ssid);
                    was_connected = true;
                    last_check_us = esp_timer_get_time();
                    s_upgrade_gap_s = APP_WIFI_UPGRADE_CHECK_S;
                }
                continue;
            }

            /* Always from the top, so recovering from an outage also recovers
             * the preference order rather than sticking on whichever network
             * happened to come back first. */
            bool got = false;
            for (int i = 0; i < s_count && !got; i++) {
                if (s_setup && ap_clients() > 0) {
                    break; /* someone joined the hotspot mid-sweep: stand still */
                }
                ESP_LOGI(TAG, "trying '%s'", s_nets[i].ssid);
                xSemaphoreTake(s_radio, portMAX_DELAY);
                got = try_connect(i);
                xSemaphoreGive(s_radio);
            }
            if (!got) {
                int pause = s_setup ? SETUP_SWEEP_PAUSE_S : APP_WIFI_SWEEP_PAUSE_S;
                ESP_LOGW(TAG, "no network answered, retrying in %d s", pause);
                vTaskDelay(pdMS_TO_TICKS(pause * 1000));
            } else {
                if (s_setup) {
                    xSemaphoreTake(s_radio, portMAX_DELAY);
                    leave_setup();
                    xSemaphoreGive(s_radio);
                }
                was_connected = true;
                last_check_us = esp_timer_get_time();
                s_upgrade_gap_s = APP_WIFI_UPGRADE_CHECK_S;
            }
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_current <= 0) {
            continue; /* already on the first choice, or not connected */
        }
        now = esp_timer_get_time();
        if (now - last_check_us < (int64_t)s_upgrade_gap_s * 1000000) {
            continue;
        }
        last_check_us = now;

        xSemaphoreTake(s_radio, portMAX_DELAY);
        int better = scan_for_better(s_current);
        if (better < 0) {
            xSemaphoreGive(s_radio);
            continue;
        }

        int previous = s_current;
        ESP_LOGI(TAG, "'%s' is back, moving up from '%s'", s_nets[better].ssid,
                 s_nets[previous].ssid);
        if (try_connect(better)) {
            xSemaphoreGive(s_radio);
            s_upgrade_gap_s = APP_WIFI_UPGRADE_CHECK_S;
            continue;
        }

        /* It was in range a moment ago and would not take us -- a stale
         * password, a full AP, whatever.
         *
         * Back off before offering it again, and keep backing off. Moving up
         * costs a working connection for a few seconds, so a preferred network
         * that is permanently visible and permanently refusing would otherwise
         * knock the ticker offline every 15 s for as long as it is switched
         * on. That is not hypothetical: one wrong character in a saved
         * password produces exactly it. Retrying an hour apart still recovers
         * within an hour of the AP being fixed, which is the right trade. */
        s_upgrade_gap_s *= 4;
        if (s_upgrade_gap_s > APP_WIFI_UPGRADE_MAX_GAP_S) {
            s_upgrade_gap_s = APP_WIFI_UPGRADE_MAX_GAP_S;
        }
        ESP_LOGW(TAG, "'%s' refused us, returning to '%s'; next look in %d s",
                 s_nets[better].ssid, s_nets[previous].ssid, s_upgrade_gap_s);
        if (!try_connect(previous)) {
            ESP_LOGW(TAG, "could not return to '%s' either", s_nets[previous].ssid);
        }
        xSemaphoreGive(s_radio);
        last_check_us = esp_timer_get_time();
    }
}

/* How a phone joining the setup hotspot finds the panel by itself -- two
 * independent routes, so either one working is enough:
 *
 * 1. Probing (every phone). Every DNS lookup is answered with our address
 *    (captive_dns), and every stray web request is redirected to the panel
 *    (web_panel.c). The address has to be a public-looking one for Android to
 *    trust the lookup at all: see WIFI_MGR_AP_IP.
 *
 * 2. DHCP option 114 (RFC 8910), which names a Captive Portal API (RFC 8908)
 *    -- a small JSON document at /capport saying "captive; the sign-in page
 *    is here". Android 11+ and iOS 14+ read it instead of guessing. It must
 *    point at that JSON, not at the page: a phone that fetches the page there
 *    finds no answer it can use.
 *
 * The DNS offer flag is OFFER_DNS (0x02); it was once passed as 1, which is
 * OFFER_ROUTER -- harmless only because CONFIG_LWIP_DHCPS_ADD_DNS offers our
 * address anyway. The option-114 URI is kept by pointer, so it is static. */
static void advertise_setup_page(void)
{
    static const char capport_uri[] = WIFI_MGR_AP_URL "capport";
    esp_netif_dhcps_stop(s_ap_netif);

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = htonl(WIFI_MGR_AP_IP);
    ip.gw.addr = htonl(WIFI_MGR_AP_IP);
    ip.netmask.addr = htonl(0xFFFFFF00u);
    esp_err_t err = esp_netif_set_ip_info(s_ap_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hotspot address rejected: %s", esp_err_to_name(err));
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = htonl(WIFI_MGR_AP_IP);
    uint8_t offer = OFFER_DNS;
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer,
                           sizeof(offer));
    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                 (void *)capport_uri, strlen(capport_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot advertise the setup page over DHCP: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(s_ap_netif);
}

esp_err_t wifi_mgr_start(const wifi_mgr_net_t *nets, int count)
{
    if (count < 0 || count > WIFI_MGR_MAX_NETS || (count > 0 && nets == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < count; i++) {
        s_nets[i] = nets[i];
    }
    s_count = count;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    /* Capitals: the screen can show nothing else, and a network name has to
     * be typed exactly as shown. */
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "VALUMI-%02X%02X", mac[4], mac[5]);

    s_events = xEventGroupCreate();
    s_radio = xSemaphoreCreateMutex();
    if (s_events == NULL || s_radio == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, APP_HOSTNAME);
    advertise_setup_page();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    /* The settings store owns the credentials. Left to its default, the
     * driver would keep its own copy in NVS as well -- one that a factory
     * reset would have to know about. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Above the app's own net task so a reconnect is not stuck behind a TLS
     * handshake, well below the render task and the driver's own tasks. */
    if (xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, APP_WIFI_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (count > 0) {
        ESP_LOGI(TAG, "%d network(s), '%s' preferred", s_count, s_nets[0].ssid);
    } else {
        ESP_LOGI(TAG, "no networks saved");
    }
    return ESP_OK;
}

const char *wifi_mgr_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *wifi_mgr_ap_password(void)
{
    return s_ap_password;
}

wifi_mgr_state_t wifi_mgr_state(void)
{
    if (s_connected) {
        return WIFI_MGR_ONLINE;
    }
    return s_setup ? WIFI_MGR_SETUP : WIFI_MGR_CONNECTING;
}

bool wifi_mgr_connected(void)
{
    return s_connected;
}

const char *wifi_mgr_ssid(void)
{
    int i = s_current >= 0 ? s_current : s_attempting;
    if (i < 0 || i >= s_count) {
        return "";
    }
    return s_nets[i].ssid;
}

uint32_t wifi_mgr_ip(void)
{
    return s_connected ? s_ip : 0;
}

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

int wifi_mgr_ap_clients(void)
{
    return ap_clients();
}

uint32_t wifi_mgr_join_count(void)
{
    return s_join_count;
}

int wifi_mgr_scan(wifi_mgr_scan_rec_t *out, int max, bool fresh)
{
    /* Bounded wait: a connection attempt can hold the radio for up to
     * APP_WIFI_ATTEMPT_TIMEOUT_S, and a panel request should answer rather
     * than hang if something is stuck. */
    if (xSemaphoreTake(s_radio, pdMS_TO_TICKS((APP_WIFI_ATTEMPT_TIMEOUT_S + 8) * 1000)) !=
        pdTRUE) {
        return -1;
    }
    int n = s_scan_cache_n;
    if (fresh || n < 0) {
        n = scan_all_locked();
    }
    if (n > max) {
        n = max;
    }
    if (n > 0) {
        memcpy(out, s_scan_cache, sizeof(*out) * (size_t)n);
    }
    xSemaphoreGive(s_radio);
    return n;
}
