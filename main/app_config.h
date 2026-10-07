/*
 * The ticker's fixed policy: timings, defaults for the owner's settings, and
 * the chart ranges. What the owner can change lives in settings.h.
 */
#pragma once

/* ---- Wi-Fi ------------------------------------------------------------- */
/* No networks are compiled in. A new ticker -- or one that has been factory
 * reset -- has none, starts its own setup hotspot, and is given them through
 * the web panel, which stores them in NVS.
 *
 * For development, a gitignored main/wifi_secrets.h may define
 *     APP_SEED_WIFI_NETWORKS   {{"ssid", "password"}, ...}
 *     APP_SEED_PANEL_PASSWORD  "ABCDE"  (5 of A-Z, 2-9; shown on screen)
 * which are written into the settings whenever there are none usable: on a
 * blank device, or if they cannot be read. The first
 * boot after a factory reset is the exception, so setup mode stays reachable
 * on a development build too. To apply the file to a ticker that already has
 * settings, erase them after flashing:
 *     idf.py -p PORT flash && python -m esptool -p PORT erase_region 0x9000 0x6000 */

/* How long without any saved network before the setup hotspot comes up as
 * well. Long enough to ride out a router reboot; short enough that a ticker
 * carried somewhere new offers itself for setup before anyone gives up. */
#define APP_SETUP_FALLBACK_S 120

/* How long to give one network before moving on. An AP that is out of range
 * refuses in well under a second, so this only bites on one that is present
 * but slow to authenticate. */
#define APP_WIFI_ATTEMPT_TIMEOUT_S 12

/* Pause after a whole sweep finds nothing, before starting again. */
#define APP_WIFI_SWEEP_PAUSE_S 5

/* While on a fallback network, how often to look for a preferred one. Each
 * check is an SSID-filtered scan, which takes the radio off channel for about
 * a second -- the ticker's poll retries absorb that, but do not make this much
 * shorter. On the first-choice network no scanning happens at all. */
#define APP_WIFI_UPGRADE_CHECK_S 15

/* Ceiling on the backoff applied when a preferred network is visible but
 * refuses us. Moving up costs the working connection for a few seconds, so
 * one that always refuses -- a stale password is the obvious way -- must not
 * be allowed to do that every 15 s forever. An hour still means recovery
 * within an hour of the AP being fixed. */
#define APP_WIFI_UPGRADE_MAX_GAP_S 3600

/* Above the app's net task (3) so a reconnect is not stuck behind a TLS
 * handshake, below the render task (10). */
#define APP_WIFI_TASK_PRIO 4

/* ---- Market data ------------------------------------------------------- */
/* The DEFAULT coin list: OKX instrument IDs and the label shown on screen for
 * each. The owner edits the real list in the web panel (up to SET_MAX_COINS,
 * labels up to SET_LABEL_MAX characters); this is what a new or
 * factory-reset ticker starts with. */
typedef struct {
    const char *inst_id; /* OKX instId, e.g. "BTC-USDT" */
    const char *label;   /* on-screen symbol, keep to 3-4 chars */
} app_coin_t;

#define APP_COINS                                                                                  \
    {                                                                                              \
        {"OKB-USDT", "OKB"}, {"BTC-USDT", "BTC"}, {"ETH-USDT", "ETH"}, {"SOL-USDT", "SOL"},        \
    }


/* ---- Defaults for the owner's settings ----------------------------------
 * Everything below in this block is only a starting value: the web panel
 * changes it at run time and the change is kept in NVS. */
/* DHCP and mDNS name: the panel is at http://valumi.local */
#define APP_HOSTNAME "valumi"
#define APP_TZ_OFFSET_MIN (8 * 60) /* UTC+8 */
#define APP_NIGHT_START_MIN (22 * 60)
#define APP_NIGHT_END_MIN (7 * 60)
#define APP_NIGHT_BRIGHTNESS 10
/* How far a page fades as it slides out (and the next fades in), percent: 100
 * fades through the background, 0 is a pure slide. 100 is the transition as
 * chosen on hardware. */
#define APP_TRANSITION_FADE 100
/* How long a page change takes, ms. 680 is the transition as chosen on
 * hardware. */
#define APP_TRANSITION_MS 680

/* Price poll cadence for the coin on screen. Coins not showing are not
 * polled at all: most of every round would go on prices nobody can see. */
#define APP_TICKER_INTERVAL_S 3

/* How long before a page transition the incoming coin is fetched, so it
 * arrives on screen already priced instead of appearing as "--". Polling then
 * continues on that coin, which is by then the one on screen. */
#define APP_PREFETCH_LEAD_S 1

/* One coin is shown full-screen at a time; this is how long each page stays
 * up before rotating to the next. A swipe restarts this from the beginning,
 * and a finger resting on the screen holds it off entirely. */
#define APP_ROTATE_INTERVAL_S 8

/* Floor on how often a swipe may trigger its own price fetch. A swipe puts a
 * coin on screen the schedule was not expecting, so its price wants
 * refreshing -- but flicking through four coins must not become four requests
 * a second. Below the normal poll interval, so a deliberate swipe still feels
 * immediate. */
#define APP_SWIPE_POLL_MIN_S 1

/* Chart ranges, cycled by long-pressing the button. Each is a bar size and
 * candle count; all are fetched every refresh so switching is instant. */
typedef struct {
    const char *bar;   /* OKX bar parameter */
    int count;         /* candles fetched */
    const char *tag;   /* status/log name */
} app_range_t;

#define APP_NUM_RANGES 3
#define APP_RANGES                                                                                 \
    {                                                                                              \
        {"1H", 24, "1D"}, {"4H", 42, "7D"}, {"1D", 30, "30D"},                                     \
    }
#define APP_DEFAULT_RANGE 1 /* 7D; the owner's choice is kept in the settings */

/* The most candles any range shows, and all ranges' candles together: each
 * coin holds exactly that many (market_start checks them against
 * APP_RANGES). */
#define APP_CHART_MAX 42
#define APP_CHART_SLOTS (24 + 42 + 30)

/* Charts are the heavy payloads -- a 42-candle response dwarfs a ticker -- and
 * none of these ranges moves visibly inside five minutes. The right edge still
 * tracks the live price between refreshes, patched from each ticker poll. */
#define APP_CANDLE_REFRESH_S 300

/* A series whose fetch failed is retried this often rather than waiting out
 * the full refresh interval. */
#define APP_CANDLE_RETRY_S 15

/* The 1D range's "1H" change is measured from the close of the one-minute
 * bar an hour ago, fetched on the chart refresh and only while that range is
 * selected. On the 5-minute refresh the hour it spans drifts between 60 and
 * 65 minutes; for a change indicator that is well inside the noise. */
#define APP_H1_BAR "1m"
#define APP_H1_RANGE 0 /* the range whose change figure it feeds: 1D */

/* ---- Display ----------------------------------------------------------- */
/* How long the address and login QR screen stays up: after connecting, and
 * after a 5-10 s button hold respectively. */
#define APP_ADDRESS_SCREEN_S 5
#define APP_LOGIN_SCREEN_S 20

#define APP_BRIGHTNESS_DEFAULT 30
#define APP_BRIGHTNESS_STEP 10

/* Data older than this is drawn dimmed, so a silent network failure cannot
 * masquerade as a live price.
 *
 * It must stay above the worst-case age of HEALTHY data. Only the on-screen
 * coin is polled, so a coin's price ages across the rest of the carousel
 * before its pre-fetch:
 *     (coins - 1) * (dwell + transition) + APP_TICKER_INTERVAL_S - APP_PREFETCH_LEAD_S
 * Coins, dwell and transition are owner settings (15 coins at 15 s with a
 * 3 s transition is 258 s), so the threshold is that figure plus
 * APP_STALE_MARGIN_S, computed at run time (coin_stale_after_s), and never
 * less than APP_STALE_AFTER_S. */
#define APP_STALE_MARGIN_S 4
#define APP_STALE_AFTER_S 30
