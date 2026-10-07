#include "web_panel.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "app_config.h"
#include "auth.h"
#include "board_orientation.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "firmware.h"
#include "market.h"
#include "memwatch.h"
#include "settings_json.h"
#include "settings_store.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t logo_png_start[] asm("_binary_logo_png_start");
extern const uint8_t logo_png_end[] asm("_binary_logo_png_end");

/* Request bodies are bounded before parsing: the server is one task with a
 * small stack on a small heap, and login is open to anyone on the network.
 * The panel's own requests fit with room to spare. */
#define MAX_JSON_BODY 4096
#define MAX_LOGIN_BODY 256
#define MAX_JSON_DEPTH 8
#define MAX_JSON_ELEMENTS 256
/* Whole-body deadlines: the server is one task, so a client that sends
 * slowly holds the entire panel for as long as it is waited on. */
#define JSON_DEADLINE_US (10 * 1000000LL)
#define OTA_DEADLINE_US (300 * 1000000LL)
/* Total time one save may spend asking OKX about new coins -- which is also
 * how long the panel is busy for. Coins not reached in time are accepted with
 * a warning, as when offline. */
#define COIN_CHECK_BUDGET_MS 40000
#define OTA_CHUNK 4096
#define COOKIE_NAME "sid"

static esp_timer_handle_t s_restart_timer;

/* ---- helpers ------------------------------------------------------------ */

static bool setup_mode(void)
{
    return wifi_mgr_state() == WIFI_MGR_SETUP;
}

static const char *mode_name(void)
{
    switch (wifi_mgr_state()) {
    case WIFI_MGR_SETUP:
        return "setup";
    case WIFI_MGR_ONLINE:
        return "online";
    default:
        return "connecting";
    }
}

/* The session id from the request's cookie, or "" if none. */
static void request_sid(httpd_req_t *req, char sid[AUTH_TOKEN_HEX + 1])
{
    sid[0] = '\0';
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len > 1024) {
        return;
    }
    char *cookie = malloc(len + 1);
    if (cookie == NULL) {
        return;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, len + 1) == ESP_OK &&
        !auth_cookie_value(cookie, COOKIE_NAME, sid, AUTH_TOKEN_HEX + 1)) {
        sid[0] = '\0';
    }
    free(cookie);
}

/* Setup mode needs no login: the hotspot's passphrase, shown only on the
 * ticker's screen, is the gate. */
static bool authorised(httpd_req_t *req)
{
    if (setup_mode()) {
        return true;
    }
    char sid[AUTH_TOKEN_HEX + 1];
    request_sid(req, sid);
    return sid[0] != '\0' && auth_check(sid);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *j)
{
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    memwatch_note("panel request", req->uri);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "error", msg);
    return send_json(req, status, j);
}

/* An error sent before the body was read. ESP_FAIL has the server close the
 * connection; ESP_OK would have it read and discard whatever body the client
 * declared, however large and however slowly sent. */
static esp_err_t refuse(httpd_req_t *req, const char *status, const char *msg)
{
    send_error(req, status, msg);
    return ESP_FAIL;
}

/* A body of panel size is read and dropped first (within the usual deadline),
 * so the 401 reaches the browser: closing on unread data makes TCP reset the
 * connection, and the page would report a network error rather than ask the
 * owner to log in again. */
static esp_err_t refuse_unauthorised(httpd_req_t *req)
{
    if (req->content_len > MAX_JSON_BODY) {
        return refuse(req, "401 Unauthorized", "log in first");
    }
    char buf[256];
    int64_t deadline = esp_timer_get_time() + JSON_DEADLINE_US;
    for (size_t left = req->content_len; left > 0;) {
        int r = httpd_req_recv(req, buf, MIN(left, sizeof(buf)));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline) {
            continue;
        }
        if (r <= 0) {
            return ESP_FAIL;
        }
        left -= (size_t)r;
    }
    return send_error(req, "401 Unauthorized", "log in first");
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, "200 OK", j);
}

/* Mutating requests must carry a JSON (or, for firmware, binary) content
 * type: a page on another site can only send the "simple" types without a
 * CORS preflight, which this server never grants. */
static bool content_type_is(httpd_req_t *req, const char *want)
{
    char ct[48];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        return false;
    }
    return strncmp(ct, want, strlen(want)) == 0;
}

/* Read and parse a JSON body of at most @p max bytes. If *out is left NULL,
 * the request has been dealt with and the handler returns what this did. */
static esp_err_t read_json(httpd_req_t *req, size_t max, cJSON **out)
{
    *out = NULL;
    if (!content_type_is(req, "application/json")) {
        return refuse(req, "415 Unsupported Media Type", "expected application/json");
    }
    if (req->content_len == 0 || req->content_len > max) {
        return refuse(req, "413 Payload Too Large", "request body missing or too large");
    }
    char *buf = malloc(req->content_len + 1);
    if (buf == NULL) {
        return refuse(req, "500 Internal Server Error", "out of memory");
    }
    int64_t deadline = esp_timer_get_time() + JSON_DEADLINE_US;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    if (settings_json_bounded(buf, got, MAX_JSON_DEPTH, MAX_JSON_ELEMENTS)) {
        *out = cJSON_Parse(buf);
    }
    memset(buf, 0, got); /* may hold passwords */
    free(buf);
    if (*out == NULL) {
        return send_error(req, "400 Bad Request", "not valid JSON, or too large");
    }
    return ESP_OK;
}

static void wipe_json_string(cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring) {
        memset(item->valuestring, 0, strlen(item->valuestring));
    }
}

/* Before a request body with Wi-Fi passwords in it is freed. */
static void wipe_network_passwords(const cJSON *body)
{
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(body, "networks"))
    {
        wipe_json_string(cJSON_GetObjectItemCaseSensitive(it, "password"));
    }
}

/* Every restart from the panel follows a request this firmware has just
 * served, which is all probation waits to see. Without this, a new image
 * restarted within its first minute would be rolled back. */
static void restart_cb(void *arg)
{
    firmware_confirm();
    settings_flush();
    esp_restart();
}

/* Restart shortly, so the response that announces it reaches the browser. */
static void restart_soon(void)
{
    esp_timer_start_once(s_restart_timer, 1500 * 1000);
}

static void set_session_cookie(httpd_req_t *req, const char *sid, char *buf, size_t n)
{
    snprintf(buf, n, COOKIE_NAME "=%s; Path=/; Max-Age=%lld; HttpOnly; SameSite=Strict", sid,
             AUTH_SESSION_IDLE_US / 1000000);
    httpd_resp_set_hdr(req, "Set-Cookie", buf);
}

/* ---- page --------------------------------------------------------------- */

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                           index_html_gz_end - index_html_gz_start);
}

static esp_err_t logo_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *)logo_png_start, logo_png_end - logo_png_start);
}

/* Anything else. In setup mode that includes every phone's "is there
 * internet?" probe, and redirecting it here is what pops the setup page open
 * by itself. */
static esp_err_t fallback_get(httpd_req_t *req)
{
    if (setup_mode()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", WIFI_MGR_AP_URL);
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

/* The Captive Portal API (RFC 8908) that DHCP option 114 points phones at. */
static esp_err_t capport_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/captive+json");
    httpd_resp_set_hdr(req, "Cache-Control", "private");
    if (setup_mode()) {
        return httpd_resp_sendstr(
            req, "{\"captive\":true,\"user-portal-url\":\"" WIFI_MGR_AP_URL "\"}");
    }
    return httpd_resp_sendstr(req, "{\"captive\":false}");
}

/* ---- auth --------------------------------------------------------------- */

static esp_err_t state_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", mode_name());
    cJSON_AddBoolToObject(j, "logged_in", authorised(req));
    cJSON_AddStringToObject(j, "hostname", APP_HOSTNAME);
    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    return send_json(req, "200 OK", j);
}

static esp_err_t login_post(httpd_req_t *req)
{
    cJSON *j;
    esp_err_t err = read_json(req, MAX_LOGIN_BODY, &j);
    if (j == NULL) {
        return err;
    }
    cJSON *pw = cJSON_GetObjectItemCaseSensitive(j, "password");
    char sid[AUTH_TOKEN_HEX + 1];
    int r = cJSON_IsString(pw) ? auth_login_password(pw->valuestring, sid) : 0;
    wipe_json_string(pw);
    cJSON_Delete(j);
    if (r < 0) {
        return send_error(req, "429 Too Many Requests",
                          "too many wrong passwords - wait 30 seconds");
    }
    if (r == 0) {
        return send_error(req, "401 Unauthorized", "wrong password");
    }
    char cookie[128];
    set_session_cookie(req, sid, cookie, sizeof(cookie));
    return send_ok(req);
}

/* GET /login?t=<token>: the QR code on the ticker's screen. */
static esp_err_t qr_login_get(httpd_req_t *req)
{
    char query[64], token[AUTH_TOKEN_HEX + 1] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "t", token, sizeof(token));
    }
    char sid[AUTH_TOKEN_HEX + 1];
    char cookie[128];
    httpd_resp_set_status(req, "302 Found");
    /* Redirect either way, so the token leaves the address bar and history
     * straight away. */
    if (auth_login_qr(token, sid)) {
        set_session_cookie(req, sid, cookie, sizeof(cookie));
        httpd_resp_set_hdr(req, "Location", "/");
    } else {
        httpd_resp_set_hdr(req, "Location", "/?qr=expired");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t logout_post(httpd_req_t *req)
{
    if (!content_type_is(req, "application/json")) {
        return refuse(req, "415 Unsupported Media Type", "expected application/json");
    }
    char sid[AUTH_TOKEN_HEX + 1];
    request_sid(req, sid);
    if (sid[0]) {
        auth_logout(sid);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE_NAME "=; Path=/; Max-Age=0; SameSite=Strict");
    return send_ok(req);
}

/* ---- settings ----------------------------------------------------------- */

/* The settings plus what the page needs to present them: whether "fixed" is
 * the only orientation, the chart range names, and every limit, so the page
 * validates by the same rules as here. */
static cJSON *settings_view(const settings_t *s)
{
    cJSON *j = settings_to_json(s);
    if (j == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(j, "orientation_sensor", board_orientation_available());
    static const app_range_t ranges[APP_NUM_RANGES] = APP_RANGES;
    cJSON *names = cJSON_AddArrayToObject(j, "range_names");
    for (int i = 0; i < APP_NUM_RANGES; i++) {
        cJSON_AddItemToArray(names, cJSON_CreateString(ranges[i].tag));
    }
    static const struct {
        const char *name;
        int value;
    } limits[] = {
        {"max_coins", SET_MAX_COINS},
        {"max_nets", SET_MAX_NETS},
        {"dwell_min", SET_DWELL_MIN_S},
        {"dwell_max", SET_DWELL_MAX_S},
        {"bright_min", SET_BRIGHT_MIN},
        {"bright_max", SET_BRIGHT_MAX},
        {"fade_max", SET_FADE_MAX},
        {"transition_min_ms", SET_TRANSITION_MIN_MS},
        {"transition_max_ms", SET_TRANSITION_MAX_MS},
        {"label_max", SET_LABEL_MAX},
        {"inst_max", SET_INST_MAX},
        {"ssid_max", SET_SSID_MAX},
        {"wifi_pw_min", SET_WIFI_PASS_MIN},
        {"wifi_pw_max", SET_WIFI_PASS_MAX},
    };
    cJSON *lim = cJSON_AddObjectToObject(j, "limits");
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        cJSON_AddNumberToObject(lim, limits[i].name, limits[i].value);
    }
    return j;
}

static esp_err_t settings_get_h(httpd_req_t *req)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    settings_t *s = settings_dup();
    if (s == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON *j = settings_view(s);
    settings_free(s);
    return send_json(req, "200 OK", j);
}

/* New coins are checked with the exchange before they are accepted, so a typo
 * is refused here rather than sitting on screen as "--" forever. When the
 * ticker is offline, or the checks run out of time, a coin is accepted on its
 * format alone and the answer says so. */
static bool check_new_coins(const settings_t *before, const settings_t *after, cJSON *warnings,
                            char *err, size_t en)
{
    int idx[SET_MAX_COINS];
    int n = settings_new_coins(before, after, idx, SET_MAX_COINS);
    int64_t deadline = esp_timer_get_time() + COIN_CHECK_BUDGET_MS * 1000LL;
    for (int k = 0; k < n; k++) {
        const char *id = after->coins[idx[k]].inst_id;
        float price;
        market_check_t r =
            market_check_coin(id, &price, (int)((deadline - esp_timer_get_time()) / 1000));
        if (r == MARKET_COIN_UNKNOWN) {
            snprintf(err, en, "the exchange does not list %s", id);
            return false;
        }
        if (r == MARKET_COIN_UNREACHABLE) {
            char w[96];
            snprintf(w, sizeof(w), "%s could not be checked with the exchange (offline or busy)", id);
            cJSON_AddItemToArray(warnings, cJSON_CreateString(w));
        }
    }
    return true;
}

static int apply_request(settings_t *s, const cJSON *j, bool import, cJSON *warnings, char *err,
                         size_t en)
{
    return import ? settings_import_json(s, j, warnings, err, en)
                  : settings_apply_json(s, j, err, en);
}

/* PUT /api/settings and POST /api/import.
 *
 * The request is applied twice: first to a trial copy, to learn which coins
 * are new and check them; then -- since the checks can take seconds -- to the
 * settings as they are by then, so the save does not undo anything changed
 * meanwhile, such as a brightness step from the button. */
static esp_err_t save_settings(httpd_req_t *req, bool import)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    cJSON *j;
    esp_err_t ret = read_json(req, MAX_JSON_BODY, &j);
    if (j == NULL) {
        return ret;
    }
    settings_t *before = settings_dup();
    settings_t *s = settings_dup();
    cJSON *warnings = cJSON_CreateArray();
    char err[160];
    if (before == NULL || s == NULL || warnings == NULL) {
        ret = send_error(req, "500 Internal Server Error", "out of memory");
        goto done;
    }
    int flags = apply_request(s, j, import, NULL, err, sizeof(err));
    if (flags > 0 && (flags & SET_CHG_COINS) &&
        !check_new_coins(before, s, warnings, err, sizeof(err))) {
        flags = -1;
    }
    if (flags >= 0) { /* even when nothing changes: an import may still warn */
        settings_get(s);
        flags = apply_request(s, j, import, warnings, err, sizeof(err));
    }
    if (flags < 0) {
        ret = send_error(req, "400 Bad Request", err);
        goto done;
    }
    if (flags > 0 && settings_update(s, SETTINGS_SAVE_NOW) != ESP_OK) {
        ret = send_error(req, "500 Internal Server Error", "could not save");
        goto done;
    }
    cJSON *view = settings_view(s);
    cJSON *resp = cJSON_CreateObject();
    if (view == NULL || resp == NULL) {
        cJSON_Delete(view);
        cJSON_Delete(resp);
        ret = send_error(req, "500 Internal Server Error", "out of memory");
        goto done;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "restart_needed", (flags & SET_CHG_NETS) != 0);
    cJSON_AddItemToObject(resp, "warnings", warnings);
    warnings = NULL;
    cJSON_AddItemToObject(resp, "settings", view);
    ret = send_json(req, "200 OK", resp);
done:
    cJSON_Delete(warnings);
    wipe_network_passwords(j);
    cJSON_Delete(j);
    settings_free(before);
    settings_free(s);
    return ret;
}

static esp_err_t settings_put(httpd_req_t *req)
{
    return save_settings(req, false);
}

static esp_err_t import_post(httpd_req_t *req)
{
    return save_settings(req, true);
}

/* ---- Wi-Fi -------------------------------------------------------------- */

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    char query[32], v[4] = "";
    bool fresh = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                 httpd_query_key_value(query, "fresh", v, sizeof(v)) == ESP_OK && v[0] == '1';
    wifi_mgr_scan_rec_t *recs = calloc(WIFI_MGR_SCAN_MAX, sizeof(*recs));
    if (recs == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int n = wifi_mgr_scan(recs, WIFI_MGR_SCAN_MAX, fresh);
    if (n < 0) {
        free(recs);
        return send_error(req, "503 Service Unavailable", "the radio is busy - try again");
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(resp, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", recs[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(e, "secure", recs[i].secure);
        cJSON_AddItemToArray(arr, e);
    }
    free(recs);
    return send_json(req, "200 OK", resp);
}

/* POST /api/wifi {networks: [...]}: save, then restart onto them. */
static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    cJSON *j;
    esp_err_t ret = read_json(req, MAX_JSON_BODY, &j);
    if (j == NULL) {
        return ret;
    }
    settings_t *s = settings_dup();
    char err[160];
    if (s == NULL) {
        ret = send_error(req, "500 Internal Server Error", "out of memory");
    } else if (settings_apply_wifi_json(s, j, err, sizeof(err)) < 0) {
        ret = send_error(req, "400 Bad Request", err);
    } else if (s->n_nets == 0) {
        ret = send_error(req, "400 Bad Request", "add at least one network");
    } else if (settings_update(s, SETTINGS_SAVE_NOW) != ESP_OK) {
        ret = send_error(req, "500 Internal Server Error", "could not save");
    } else {
        ESP_LOGI(TAG, "Wi-Fi settings saved; restarting");
        ret = send_ok(req);
        restart_soon();
    }
    wipe_network_passwords(j);
    cJSON_Delete(j);
    settings_free(s);
    return ret;
}

/* ---- status ------------------------------------------------------------- */

static esp_err_t status_get(httpd_req_t *req)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    char ip[16] = "";
    if (wifi_mgr_ip()) {
        settings_format_ipv4(wifi_mgr_ip(), ip, sizeof(ip));
    }
    cJSON *j = cJSON_CreateObject();
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(w, "mode", mode_name());
    cJSON_AddStringToObject(w, "ssid", wifi_mgr_connected() ? wifi_mgr_ssid() : "");
    cJSON_AddStringToObject(w, "ip", ip);
    cJSON_AddNumberToObject(w, "rssi", wifi_mgr_rssi());

    cJSON *sys = cJSON_AddObjectToObject(j, "system");
    cJSON_AddStringToObject(sys, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(sys, "heap_min_free",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    /* Not shown on the page: the device test reads it to see an update land. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(sys, "partition", run ? run->label : "?");

    cJSON_AddBoolToObject(j, "clock_synced", market_clock_synced());
    return send_json(req, "200 OK", j);
}

/* ---- export ------------------------------------------------------------- */

static esp_err_t export_get(httpd_req_t *req)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    settings_t *s = settings_dup();
    if (s == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON *j = settings_to_export_json(s);
    settings_free(s);
    /* The file name comes from here: a phone's sign-in window, where setup
     * mode opens, ignores the one a page asks for. */
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"valumi-settings.json\"");
    return send_json(req, "200 OK", j);
}

/* ---- firmware ----------------------------------------------------------- */

/* Where an app's description sits in its image. */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

/* Refuses another app's firmware, which would boot, find no settings it
 * understands, and strand the ticker until someone found a USB cable. */
static bool image_is_ours(const uint8_t *buf, size_t len, char *why, size_t wn)
{
    if (len < APP_DESC_OFFSET + sizeof(esp_app_desc_t)) {
        snprintf(why, wn, "file too short to be firmware");
        return false;
    }
    if (buf[0] != ESP_IMAGE_HEADER_MAGIC) {
        snprintf(why, wn, "not a valumi firmware file");
        return false;
    }
    const esp_image_header_t *h = (const esp_image_header_t *)buf;
    if (h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        snprintf(why, wn, "firmware is for a different chip");
        return false;
    }
    esp_app_desc_t desc;
    memcpy(&desc, buf + APP_DESC_OFFSET, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(why, wn, "firmware has no app description");
        return false;
    }
    if (strncmp(desc.project_name, esp_app_get_description()->project_name,
                sizeof(desc.project_name)) != 0) {
        snprintf(why, wn, "firmware is for '%.32s', not this ticker", desc.project_name);
        return false;
    }
    return true;
}

/* A full flash image -- bootloader, partition table and app, for a first
 * install over USB -- starts with the bootloader, an ESP image with no app
 * description; its app sits where the first app slot begins. Accepting it
 * here means one download serves both a first install and updates. */
static bool is_full_image(const uint8_t *buf, size_t len, size_t total, size_t app_offset)
{
    const esp_image_header_t *h = (const esp_image_header_t *)buf;
    if (len < APP_DESC_OFFSET + sizeof(uint32_t) || buf[0] != ESP_IMAGE_HEADER_MAGIC ||
        h->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID || total <= app_offset) {
        return false;
    }
    /* A bootloader has no app description where an app keeps one; another
     * project's app does, and gets its own refusal instead. */
    uint32_t magic;
    memcpy(&magic, buf + APP_DESC_OFFSET, sizeof(magic));
    return magic != ESP_APP_DESC_MAGIC_WORD;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    if (!content_type_is(req, "application/octet-stream")) {
        return refuse(req, "415 Unsupported Media Type", "expected application/octet-stream");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return refuse(req, "500 Internal Server Error", "no update partition");
    }
    /* Where the app sits in a full image: everything before it is the
     * bootloader and partition table. */
    const esp_partition_t *first =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    size_t app_offset = first ? first->address : 0;
    if (req->content_len == 0 || req->content_len > app_offset + part->size) {
        return refuse(req, "413 Payload Too Large", "firmware missing or too large");
    }
    uint8_t *buf = malloc(OTA_CHUNK);
    if (buf == NULL) {
        return refuse(req, "500 Internal Server Error", "out of memory");
    }
    /* An image still on probation cannot start another update, and serving
     * this request is proof enough that it works. */
    firmware_confirm();

    ESP_LOGI(TAG, "firmware upload: %u bytes to %s", (unsigned)req->content_len, part->label);
    esp_ota_handle_t ota = 0;
    bool begun = false;
    size_t got = 0;  /* bytes received */
    size_t fill = 0; /* of which still in buf, not yet written */
    size_t app_at = 0; /* in a full image, where the app starts */
    int64_t deadline = esp_timer_get_time() + OTA_DEADLINE_US;
    char why[96] = "";
    esp_err_t err = ESP_OK;
    /* Gathered before checking: the first read can return just the few body
     * bytes that arrived with the headers. */
    const size_t head =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

    while (got < req->content_len) {
        size_t want = MIN(OTA_CHUNK - fill, req->content_len - got);
        if (got < app_at) {
            want = MIN(want, app_at - got); /* never read past the app's start */
        }
        int r = httpd_req_recv(req, (char *)buf + fill, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline) {
            continue;
        }
        if (r <= 0) {
            snprintf(why, sizeof(why), "upload interrupted");
            err = ESP_FAIL;
            break;
        }
        got += (size_t)r;
        fill += (size_t)r;
        if (got <= app_at) {
            fill = 0; /* the bootloader and partition table: not ours to write */
            continue;
        }
        if (!begun) {
            if (fill < head && got < req->content_len) {
                continue;
            }
            if (!image_is_ours(buf, fill, why, sizeof(why))) {
                if (app_at == 0 && app_offset > got &&
                    is_full_image(buf, fill, req->content_len, app_offset)) {
                    app_at = app_offset;
                    fill = 0;
                    continue;
                }
                err = ESP_ERR_INVALID_ARG;
                break;
            }
            err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (err != ESP_OK) {
                snprintf(why, sizeof(why), "could not start: %s", esp_err_to_name(err));
                break;
            }
            begun = true;
        }
        err = esp_ota_write(ota, buf, fill);
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "write failed: %s", esp_err_to_name(err));
            break;
        }
        fill = 0;
    }
    free(buf);

    if (err == ESP_OK) {
        err = esp_ota_end(ota); /* verifies the image checksum */
        begun = false;
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "image failed verification: %s", esp_err_to_name(err));
        }
    }
    if (begun) {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
        if (err != ESP_OK) {
            snprintf(why, sizeof(why), "could not select new firmware: %s", esp_err_to_name(err));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "firmware upload refused: %s", why);
        /* Possibly before the whole body arrived: close, don't drain. */
        return refuse(req, "400 Bad Request", why);
    }
    ESP_LOGI(TAG, "firmware accepted; restarting into %s", part->label);
    esp_err_t ret = send_ok(req);
    restart_soon();
    return ret;
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (!authorised(req)) {
        return refuse_unauthorised(req);
    }
    if (!content_type_is(req, "application/json")) {
        return refuse(req, "415 Unsupported Media Type", "expected application/json");
    }
    esp_err_t ret = send_ok(req);
    restart_soon();
    return ret;
}

/* ---- start -------------------------------------------------------------- */

esp_err_t web_panel_start(void)
{
    const esp_timer_create_args_t targs = {.callback = restart_cb, .name = "restart"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_restart_timer));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 20;
    cfg.stack_size = 6144;
    /* Few sockets: every one is RAM, and one phone opens several. Purging the
     * least recently used keeps a new visitor from being turned away. */
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server failed to start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/login", .method = HTTP_GET, .handler = qr_login_get},
        {.uri = "/capport", .method = HTTP_GET, .handler = capport_get},
        {.uri = "/logo.png", .method = HTTP_GET, .handler = logo_get},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/login", .method = HTTP_POST, .handler = login_post},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_post},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_h},
        {.uri = "/api/settings", .method = HTTP_PUT, .handler = settings_put},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/export", .method = HTTP_GET, .handler = export_get},
        {.uri = "/api/import", .method = HTTP_POST, .handler = import_post},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post},
        /* Last: wildcard, so it only catches what nothing above matched. */
        {.uri = "/*", .method = HTTP_GET, .handler = fallback_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
    ESP_LOGI(TAG, "panel on port 80");
    return ESP_OK;
}
