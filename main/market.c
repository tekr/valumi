#include "market.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "carousel.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "market_core.h"
#include "memwatch.h"
#include "okx_client.h"
#include "settings_store.h"
#include "wifi_mgr.h"

static const char *TAG = "market";

/* Chart work per coin: one job per range, then the 1H baseline. */
#define H1_JOB APP_NUM_RANGES
#define NUM_JOBS (APP_NUM_RANGES + 1)

typedef struct {
    set_coin_t cfg;
    okx_ticker_t ticker;
    int64_t ticker_at_us; /* 0 = no data yet */
    series_t charts[APP_NUM_RANGES]; /* each holding its share of slots */
    candle_t slots[APP_CHART_SLOTS];
    float h1_base; /* close of the one-minute bar an hour ago; 0 = unknown */
    /* Kept with the coin, so re-ordering the list carries the schedule along
     * with it. */
    int64_t job_ok_us[NUM_JOBS];
    int64_t job_try_us[NUM_JOBS];
} coin_t;

static const app_range_t k_ranges[APP_NUM_RANGES] = APP_RANGES;

static coin_t *coin_new(void)
{
    coin_t *c = calloc(1, sizeof(*c));
    if (c != NULL) {
        candle_t *v = c->slots;
        for (int r = 0; r < APP_NUM_RANGES; r++) {
            c->charts[r] = (series_t){.v = v, .cap = k_ranges[r].count};
            v += k_ranges[r].count;
        }
    }
    return c;
}

/* Guards the coin list, the coins' data and the progress line. The list
 * itself only changes on the net task (or before it starts), which therefore
 * reads it unlocked. */
static SemaphoreHandle_t s_lock;
/* Each coin is its own ~2 KB allocation, so a new list is a shuffle of
 * pointers and kept coins keep their charts. */
static coin_t *s_coins[SET_MAX_COINS];
static int s_num_coins;
static uint32_t s_coins_gen;
static int s_page_ms; /* time per coin plus its slide in */
static char s_progress[56] = "STARTING";

/* Net task only, once it is running. */
static int s_range;
static uint32_t s_cfg_gen;
static int64_t s_cfg_retry_us; /* after running out of memory for a coin */

static volatile bool s_fetch_ok;
static volatile bool s_clock_ok;

static void progress(const char *fmt, ...)
{
    char buf[sizeof(s_progress)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(s_progress, buf);
    xSemaphoreGive(s_lock);
}

void market_progress(char *out, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_progress, n);
    xSemaphoreGive(s_lock);
}

bool market_clock_synced(void)
{
    return s_clock_ok;
}

bool market_fetch_ok(void)
{
    return s_fetch_ok;
}

/* ---- coin list ---------------------------------------------------------- */

/* Bring the coin list, dwell and range into line with the settings. Kept
 * coins keep everything they had; the coin on screen stays on screen if it
 * is still listed. */
static void sync_settings(void)
{
    uint32_t g = settings_generation();
    int64_t now = esp_timer_get_time();
    if (g == s_cfg_gen && (s_cfg_retry_us == 0 || now < s_cfg_retry_us)) {
        return;
    }
    s_cfg_gen = g;
    s_cfg_retry_us = 0;
    settings_t *cfg = settings_dup();
    if (cfg == NULL) {
        s_cfg_retry_us = now + 5000000;
        return;
    }
    s_range = cfg->range;

    const char *old_ids[SET_MAX_COINS], *new_ids[SET_MAX_COINS];
    for (int j = 0; j < s_num_coins; j++) {
        old_ids[j] = s_coins[j]->cfg.inst_id;
    }
    for (int i = 0; i < cfg->n_coins; i++) {
        new_ids[i] = cfg->coins[i].inst_id;
    }
    int map[SET_MAX_COINS], new_of_old[SET_MAX_COINS];
    bool same = coin_plan(old_ids, s_num_coins, new_ids, cfg->n_coins, map, new_of_old);

    /* Allocate every new coin before touching anything, so running out of
     * memory leaves the old list whole. */
    coin_t *next[SET_MAX_COINS] = {0};
    for (int i = 0; i < cfg->n_coins; i++) {
        next[i] = map[i] >= 0 ? s_coins[map[i]] : coin_new();
        if (next[i] == NULL) {
            ESP_LOGE(TAG, "no memory for coin %s; retrying in 5 s", cfg->coins[i].inst_id);
            for (int k = 0; k < i; k++) {
                if (map[k] < 0) {
                    free(next[k]);
                }
            }
            s_cfg_retry_us = now + 5000000;
            settings_free(cfg);
            return;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < cfg->n_coins; i++) {
        next[i]->cfg = cfg->coins[i]; /* a label edit lands here too */
    }
    for (int j = 0; j < s_num_coins; j++) {
        bool kept = false;
        for (int i = 0; i < cfg->n_coins; i++) {
            kept = kept || map[i] == j;
        }
        if (!kept) {
            free(s_coins[j]);
        }
    }
    int n_old = s_num_coins;
    memcpy(s_coins, next, sizeof(s_coins));
    s_num_coins = cfg->n_coins;
    s_page_ms = cfg->dwell_s * 1000 + cfg->transition_ms;
    if (!same) {
        s_coins_gen++;
    }
    /* Under s_lock, so the renderer never sees the new list with the old
     * carousel or the reverse. */
    bool first = n_old == 0; /* at boot: no page to carry over */
    carousel_configure(s_num_coins, cfg->dwell_s, same || first ? NULL : new_of_old);
    xSemaphoreGive(s_lock);
    settings_free(cfg);
}

int market_snapshot(ui_coin_t *out, int range, uint32_t *gen)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_num_coins;
    *gen = s_coins_gen;
    int64_t stale_us =
        coin_stale_after_s(n, s_page_ms, APP_TICKER_INTERVAL_S, APP_PREFETCH_LEAD_S,
                           APP_STALE_MARGIN_S, APP_STALE_AFTER_S) *
        1000000LL;
    for (int i = 0; i < n; i++) {
        const coin_t *c = s_coins[i];
        const series_t *chart = &c->charts[range];
        ui_coin_t *o = &out[i];
        strlcpy(o->label, c->cfg.label, sizeof(o->label));
        o->last = c->ticker_at_us ? c->ticker.last : 0.0f;
        o->open24h = c->ticker.open24h;
        for (int k = 0; k < chart->n; k++) {
            o->closes[k] = chart->v[k].close;
            o->highs[k] = chart->v[k].high;
            o->lows[k] = chart->v[k].low;
        }
        o->n_closes = chart->n;
        o->range = range;
        o->h1_base = c->h1_base;
        /* End the chart at the live price, so the line never disagrees with
         * the number above it. On the copy: the stored candles stay exchange
         * data, so the merge never mistakes a ticker price for a bar close. */
        int last = o->n_closes - 1;
        if (last >= 0 && c->ticker_at_us != 0) {
            float p = c->ticker.last;
            o->closes[last] = p;
            o->highs[last] = p > o->highs[last] ? p : o->highs[last];
            o->lows[last] = p < o->lows[last] ? p : o->lows[last];
        }
        o->stale = c->ticker_at_us != 0 && now - c->ticker_at_us > stale_us;
        o->flash = 0.0f;
    }
    xSemaphoreGive(s_lock);
    return n;
}

/* ---- fetching ----------------------------------------------------------- */

static bool fetch_chart(coin_t *c, int r)
{
    series_t *dst = &c->charts[r];
    okx_candle_t batch[APP_CHART_MAX];
    int n = 0;
    int64_t confirmed = 0;

    /* Only bars newer than the newest closed one held: older bars never
     * change, and this takes a refresh from ~42 candles to one or two. No
     * special case when the batch fills the limit: OKX truncates from the
     * old end, and the limit is the chart width, so a full batch is exactly
     * the bars to keep. */
    int64_t before = dst->confirmed_ts;
    if (okx_fetch_candles(c->cfg.inst_id, k_ranges[r].bar, dst->cap, before, 0, batch, &n,
                          &confirmed) != ESP_OK) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (before == 0) {
        dst->n = 0; /* a snapshot replaces whatever was held */
    }
    series_merge(dst, batch, n);
    /* Most batches hold only the forming bar; keeping the old value then is
     * what keeps the next fetch incremental. */
    if (confirmed != 0) {
        dst->confirmed_ts = confirmed;
    }
    xSemaphoreGive(s_lock);
    return true;
}

static bool fetch_h1_base(coin_t *c)
{
    okx_candle_t bar;
    int n = 0;
    int64_t confirmed;
    int64_t hour_ago_ms = (int64_t)time(NULL) * 1000 - 3600 * 1000LL;
    if (okx_fetch_candles(c->cfg.inst_id, APP_H1_BAR, 1, 0, hour_ago_ms, &bar, &n, &confirmed) !=
        ESP_OK) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c->h1_base = bar.close;
    xSemaphoreGive(s_lock);
    return true;
}

static bool job_due(const coin_t *c, int job, int64_t now)
{
    if (job == H1_JOB && s_range != APP_H1_RANGE) {
        return false;
    }
    int64_t ok = c->job_ok_us[job];
    int64_t tried = c->job_try_us[job];
    bool stale = ok == 0 || now - ok >= APP_CANDLE_REFRESH_S * 1000000LL;
    bool cooled = tried == 0 || now - tried >= APP_CANDLE_RETRY_S * 1000000LL;
    return stale && cooled;
}

/* Do at most one chart job. Returning after a single request is what keeps a
 * refresh sweep off the back of a price poll: the scheduler re-reads the
 * clock between each. Returns false when no chart work is outstanding. */
static bool candle_step(int64_t now, bool announce)
{
    for (int i = 0; i < s_num_coins; i++) {
        coin_t *c = s_coins[i];
        for (int job = 0; job < NUM_JOBS; job++) {
            if (!job_due(c, job, now)) {
                continue;
            }
            const char *tag = job == H1_JOB ? "1H" : k_ranges[job].tag;
            if (announce) {
                progress("CHART %s %s", c->cfg.label, tag);
            }
            c->job_try_us[job] = now;
            bool ok = job == H1_JOB ? fetch_h1_base(c) : fetch_chart(c, job);
            memwatch_note("chart fetch", c->cfg.inst_id);
            if (ok) {
                c->job_ok_us[job] = now;
            } else {
                ESP_LOGW(TAG, "%s %s: candle fetch failed, HTTP %d", c->cfg.inst_id, tag,
                         okx_last_http_status());
                if (announce) {
                    progress("CHART %s %s FAILED: HTTP %d", c->cfg.label, tag,
                             okx_last_http_status());
                }
            }
            return true;
        }
    }
    return false;
}

static bool fetch_ticker(coin_t *c)
{
    okx_ticker_t t;
    if (okx_fetch_ticker(c->cfg.inst_id, &t) != ESP_OK) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    c->ticker = t;
    c->ticker_at_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    return true;
}

/* ---- coin checks for the panel ------------------------------------------ */

/* The OKX client is single-threaded, so the panel leaves its question here
 * and the net task answers between requests.
 *
 * Every question carries a sequence number and every answer echoes it: a
 * question abandoned on timeout may still be answered later, and without the
 * number that answer would be taken for the NEXT question's -- a typo
 * accepted on the strength of another coin's price. */
static SemaphoreHandle_t s_check_mutex; /* one question at a time */
static SemaphoreHandle_t s_check_done;
static portMUX_TYPE s_check_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_check_inst[SET_INST_MAX + 1];
static uint32_t s_check_seq;  /* of the question being asked, 0 = none */
static uint32_t s_answer_seq; /* of the answer below */
static market_check_t s_check_result;
static float s_check_price;

static void answer_coin_check(void)
{
    char inst[SET_INST_MAX + 1];
    portENTER_CRITICAL(&s_check_mux);
    uint32_t seq = s_check_seq;
    strlcpy(inst, s_check_inst, sizeof(inst));
    s_check_seq = 0; /* taken */
    portEXIT_CRITICAL(&s_check_mux);
    if (seq == 0) {
        return;
    }
    okx_ticker_t t = {0};
    esp_err_t err = s_clock_ok ? okx_fetch_ticker(inst, &t) : ESP_FAIL;
    memwatch_note("coin check", inst);
    portENTER_CRITICAL(&s_check_mux);
    s_check_result = err == ESP_OK              ? MARKET_COIN_OK
                     : err == ESP_ERR_NOT_FOUND ? MARKET_COIN_UNKNOWN
                                                : MARKET_COIN_UNREACHABLE;
    s_check_price = t.last;
    s_answer_seq = seq;
    portEXIT_CRITICAL(&s_check_mux);
    xSemaphoreGive(s_check_done);
}

market_check_t market_check_coin(const char *inst_id, float *price, int timeout_ms)
{
    if (!wifi_mgr_connected()) {
        return MARKET_COIN_UNREACHABLE;
    }
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    /* Just after boot the network is up but the clock is not, and TLS cannot
     * start until it is. Answering "unreachable" then would let a typo be
     * saved unchecked in the first seconds after a restart, so wait for the
     * clock -- normally a few seconds -- inside the budget. */
    while (!s_clock_ok && wifi_mgr_connected() && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    int left_ms = (int)((deadline - esp_timer_get_time()) / 1000);
    if (!s_clock_ok || left_ms <= 0 ||
        xSemaphoreTake(s_check_mutex, pdMS_TO_TICKS(left_ms)) != pdTRUE) {
        return MARKET_COIN_UNREACHABLE;
    }
    static uint32_t next_seq;
    if (++next_seq == 0) {
        next_seq = 1;
    }
    uint32_t seq = next_seq;
    portENTER_CRITICAL(&s_check_mux);
    strlcpy(s_check_inst, inst_id, sizeof(s_check_inst));
    s_check_seq = seq;
    portEXIT_CRITICAL(&s_check_mux);

    market_check_t r = MARKET_COIN_UNREACHABLE;
    for (;;) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0 ||
            xSemaphoreTake(s_check_done, pdMS_TO_TICKS(left_us / 1000 + 1)) != pdTRUE) {
            break;
        }
        portENTER_CRITICAL(&s_check_mux);
        bool mine = s_answer_seq == seq;
        if (mine) {
            r = s_check_result;
            *price = s_check_price;
        }
        portEXIT_CRITICAL(&s_check_mux);
        if (mine) {
            break;
        }
    }
    /* Withdraw the question if it was never taken, so the net task does not
     * spend a request on something nobody is waiting for. */
    portENTER_CRITICAL(&s_check_mux);
    if (s_check_seq == seq) {
        s_check_seq = 0;
    }
    portEXIT_CRITICAL(&s_check_mux);
    xSemaphoreGive(s_check_mutex);
    return r;
}

/* ---- net task ----------------------------------------------------------- */

static void between_requests(void)
{
    sync_settings();
    answer_coin_check();
}

static void net_task(void *arg)
{
    /* TLS checks certificates against the wall clock, which starts at 1970.
     * Waited on in one-second steps so the panel is still answered meanwhile
     * -- with "cannot check", which is the truth. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));
    progress("SYNC CLOCK - NTP");
    for (int waited_s = 1; esp_netif_sntp_sync_wait(pdMS_TO_TICKS(1000)) != ESP_OK; waited_s++) {
        between_requests();
        if (waited_s % 15 == 0) {
            progress("SYNC CLOCK - NTP TRY %d", waited_s / 15 + 1);
            ESP_LOGW(TAG, "SNTP not synced yet, retrying");
        }
    }
    s_clock_ok = true;
    ESP_LOGI(TAG, "time synced");

    /* Fill every chart before the first price, so the splash gives way to a
     * complete page rather than a bare number over an empty chart. */
    do {
        between_requests();
    } while (candle_step(esp_timer_get_time(), true));

    const int64_t lead_us = APP_PREFETCH_LEAD_S * 1000000LL;
    const int64_t poll_us = APP_TICKER_INTERVAL_S * 1000000LL;
    const int64_t swipe_poll_us = APP_SWIPE_POLL_MIN_S * 1000000LL;
    int64_t last_poll_us = 0;
    uint32_t prefetched_for = 0; /* carousel turn */
    bool prefetched = false;
    int last_target = -1;
    uint32_t last_gen = s_coins_gen;

    for (;;) {
        between_requests();
        if (s_coins_gen != last_gen) {
            /* Indices mean different coins now. */
            last_gen = s_coins_gen;
            last_target = -1;
            prefetched = false;
        }
        int64_t now = esp_timer_get_time();

        /* Only the coin on screen is worth polling. Just before each page
         * change the incoming coin is fetched instead, so it arrives priced.
         * A held page keeps pushing its deadline forward, so the pre-fetch
         * window never opens under a finger. */
        carousel_view_t car;
        carousel_get(&car);
        /* With one coin there is no next page: the window would open and
         * never close, and the coin would never be polled again. */
        bool prefetching = car.next_page != car.page && car.due_us - now <= lead_us;
        int target = prefetching ? car.next_page : car.page;
        if (target >= s_num_coins) {
            target = 0;
        }
        if (s_num_coins == 0) { /* only if out of memory at boot: retried above */
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        coin_t *c = s_coins[target];

        bool due = prefetching ? !prefetched || prefetched_for != car.turn
                               : now - last_poll_us >= poll_us;
        /* A coin that has never reported cannot wait for its slot -- it would
         * reach the screen as "--". Still spaced by the poll interval, so a
         * coin the exchange refuses cannot become a hot loop. */
        if (c->ticker_at_us == 0 && now - last_poll_us >= poll_us) {
            due = true;
        }
        /* A swipe brings on a coin the schedule was not expecting, whose price
         * may be a whole carousel old. Refresh it now, on its own floor, so
         * flicking through the coins costs a bounded number of requests. */
        if (target != last_target && now - last_poll_us >= swipe_poll_us) {
            due = true;
        }

        if (due) {
            if (c->ticker_at_us == 0) {
                progress("PRICE %s", c->cfg.label);
            }
            last_poll_us = now; /* start-to-start, so the cadence holds */
            last_target = target;
            if (prefetching) {
                prefetched_for = car.turn;
                prefetched = true;
            }
            s_fetch_ok = fetch_ticker(c);
            memwatch_note("price fetch", c->cfg.inst_id);
            if (!s_fetch_ok) {
                ESP_LOGW(TAG, "%s: price fetch failed, HTTP %d", c->cfg.inst_id,
                         okx_last_http_status());
                progress("PRICE FAILED: HTTP %d - RETRYING", okx_last_http_status());
            }
            continue;
        }
        if (candle_step(now, false)) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void market_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_check_mutex = xSemaphoreCreateMutex();
    s_check_done = xSemaphoreCreateBinary();
    assert(s_lock && s_check_mutex && s_check_done);
    int slots = 0;
    for (int r = 0; r < APP_NUM_RANGES; r++) {
        assert(k_ranges[r].count <= APP_CHART_MAX);
        slots += k_ranges[r].count;
    }
    assert(slots == APP_CHART_SLOTS);
    sync_settings(); /* the coin list, before anything renders it */
    /* 12 KB: the TLS handshake runs on this task, and mbedTLS is the stack
     * hog of the whole firmware. Priority below the render task's: see
     * app_main. */
    xTaskCreate(net_task, "net", 12288, NULL, 3, NULL);
}
