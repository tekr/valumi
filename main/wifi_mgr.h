/*
 * Wi-Fi: a priority-ordered list of networks, and a setup hotspot for when
 * there are none or none answer.
 *
 * The ticker moves between a desk and an office, so it is given several
 * networks and always wants the best one available. It sweeps the list in
 * order until something answers, and once it is on anything other than the
 * first choice it keeps looking for a better one and moves up when it appears.
 *
 * Setup mode: with no networks at all, or none answering for
 * APP_SETUP_FALLBACK_S, the ticker also becomes an access point (WPA2, the
 * passphrase shown on its screen) with a captive portal, so a phone that joins
 * is taken straight to the web panel. It keeps trying the saved networks in
 * the background -- but only while nobody is connected to the hotspot, since
 * joining another network can move the radio's channel and the hotspot with
 * it -- and leaves setup mode as soon as one answers.
 *
 * Deliberately kept free of the settings module: the caller supplies the
 * list, so this module has no opinion about where networks come from.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MGR_MAX_NETS 4
/* Most networks one scan reports (one entry per name). */
#define WIFI_MGR_SCAN_MAX 24
/* The setup hotspot's own address, host order: 4.3.2.1, with a /24 around it.
 *
 * Deliberately NOT a private address. Android checks for internet by looking
 * up connectivitycheck.gstatic.com and fetching a page from it; if that name
 * resolves to a private address (192.168.x.x and friends), current Android
 * decides the network is rewriting DNS, reports "no internet" and never makes
 * the request -- so it never meets the redirect that would have told it
 * "sign-in page here". Measured on a Galaxy Z Fold 7: the lookup, then
 * nothing. With a public-looking address it fetches, gets redirected, and
 * offers the setup page. The hotspot reaches nothing beyond the ticker, so
 * borrowing the address harms no one (WLED does the same, for the same
 * reason). */
#define WIFI_MGR_AP_IP 0x04030201u
#define WIFI_MGR_AP_ADDR "4.3.2.1"
#define WIFI_MGR_AP_URL "http://" WIFI_MGR_AP_ADDR "/"

typedef struct {
    char ssid[33];
    char password[64]; /* empty: open network */
    bool static_ip;
    uint32_t ip, mask, gw, dns; /* host order; dns 0 = use the gateway */
} wifi_mgr_net_t;


typedef enum {
    WIFI_MGR_CONNECTING, /* trying the saved networks */
    WIFI_MGR_ONLINE,     /* on one of them, with an address */
    WIFI_MGR_SETUP,      /* the setup hotspot is up */
} wifi_mgr_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_mgr_scan_rec_t;

/**
 * @brief Bring up Wi-Fi and start working through @p nets.
 *
 * @param nets  most-preferred first; copied.
 * @param count 0..WIFI_MGR_MAX_NETS. 0 goes straight to setup mode.
 */
esp_err_t wifi_mgr_start(const wifi_mgr_net_t *nets, int count);

/** The setup hotspot's name ("VALUMI-" plus the end of the MAC, so two
 * tickers side by side differ) and its passphrase, which is new each time
 * setup mode starts: one seen on the screen once is no use later. */
const char *wifi_mgr_ap_ssid(void);
const char *wifi_mgr_ap_password(void);

wifi_mgr_state_t wifi_mgr_state(void);

/** True while associated with an IP address. */
bool wifi_mgr_connected(void);


/** The network we are on, or the one being tried. Never NULL. */
const char *wifi_mgr_ssid(void);

/** Station address, host order; 0 when not connected. */
uint32_t wifi_mgr_ip(void);

/** Signal strength of the current network, dBm; 0 when not connected. */
int wifi_mgr_rssi(void);

/** Phones and laptops on the setup hotspot right now. */
int wifi_mgr_ap_clients(void);

/** Bumped every time a network is joined; a change means "show the
 * address screen". */
uint32_t wifi_mgr_join_count(void);

/**
 * @brief Networks in range, strongest first, one entry per name.
 *
 * Works in setup mode with a phone connected: the scan returns to the
 * hotspot's channel between the channels it visits, so the phone stays on.
 * Blocks for a few seconds (longer if a connection attempt is in progress).
 *
 * @param fresh false returns the last result if there is one.
 * @return entries written, or -1 if the radio could not scan.
 */
int wifi_mgr_scan(wifi_mgr_scan_rec_t *out, int max, bool fresh);

#ifdef __cplusplus
}
#endif
