#include "okx_client.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "sh2lib.h"
#include "esp_log.h"
#include "miniz.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "okx";

/* Upper bound on an inflated response. The largest real one is a 60-candle
 * 1m snapshot, 6.7 KB (measured); the buffer itself is sized to each response. */
#define RESPONSE_BUF_SIZE (12 * 1024)
/* Wire buffer, allocated per request. Holds the body as it arrived, so it only
 * needs to cover the largest gzipped response (a full candle snapshot,
 * 2.3 KB for 60 one-minute candles, measured). */
#define WIRE_BUF_SIZE (6 * 1024)
#define HTTP_TIMEOUT_MS 8000
#define BASE_URL "https://www.okx.com"

static struct sh2lib_handle s_h2;
static bool s_h2_up;      /* session established */
static int s_wire_len;    /* bytes collected for the current response */
static bool s_stream_done;
static char *s_buf;  /* JSON, inflated if the response was compressed */
static char *s_wire; /* bytes as they arrived */
static bool s_gzip;  /* current response is gzip encoded */
static int s_last_status;

/* Cloudflare hands out a __cf_bm cookie and re-issues it on every response we
 * do not echo -- 317 bytes each time, a third of all response headers. Echoing
 * it back stops the re-issue. Kept in a static because client_destroy() runs on
 * any transport blip and takes the handle's header state with it. */
static char s_cookie[288]; /* the value alone runs ~210 chars */

/* HTTP/2 delivers the status as the :status pseudo-header rather than a
 * status line. */
static void h2_on_header(const char *name, const char *value)
{
    if (strcmp(name, ":status") == 0) {
        s_last_status = atoi(value);
        return;
    }
    if (strcasecmp(name, "content-encoding") == 0 && strcasecmp(value, "gzip") == 0) {
        s_gzip = true;
        return;
    }
    if (strcasecmp(name, "set-cookie") == 0 && strncmp(value, "__cf_bm=", 8) == 0) {
        const char *end = strchr(value, ';');
        size_t len = end ? (size_t)(end - value) : strlen(value);
        if (len < sizeof(s_cookie)) {
            memcpy(s_cookie, value, len);
            s_cookie[len] = '\0';
        } else {
            ESP_LOGE(TAG, "cf cookie too long for buffer: %u B", (unsigned)len);
        }
    }
}

/* Body chunks. Collected whole rather than streamed: the JSON parser and the
 * inflater both want a complete buffer. */
static int h2_on_data(struct sh2lib_handle *hd, const char *data, size_t len, int flags)
{
    if (len > 0) {
        if (s_wire != NULL && s_wire_len + (int)len < WIRE_BUF_SIZE) {
            memcpy(s_wire + s_wire_len, data, len);
            s_wire_len += (int)len;
        } else {
            s_wire_len = WIRE_BUF_SIZE; /* mark overflow */
        }
    }
    if (flags & DATA_RECV_RST_STREAM) {
        s_stream_done = true;
    }
    return 0;
}

int okx_last_http_status(void)
{
    return s_last_status;
}

/* The three buffers -- wire, inflater, body -- live only while a response is
 * being handled, and never during a TLS handshake: http_get() connects
 * first and allocates after. Held permanently they were ~29 KB that the
 * handshake had to find room around, and the handshake is the moment the
 * heap is at its lowest. The body is sized to the response (gzip records its
 * uncompressed length) rather than the worst case. */
static void body_free(void)
{
    free(s_buf);
    s_buf = NULL;
}

static void client_destroy(void)
{
    if (s_h2_up) {
        sh2lib_free(&s_h2);
        s_h2_up = false;
    }
}

/* Inflate a gzip member from s_wire into s_buf. Returns the inflated length,
 * or -1. ESP-ROM carries miniz, so this costs no component and no flash. */
static int gunzip(int len)
{
    /* gzip header: magic, CM, FLG, MTIME[4], XFL, OS, then optional fields
     * selected by FLG. tinfl wants the raw deflate stream that follows. */
    if (len < 18 || (uint8_t)s_wire[0] != 0x1f || (uint8_t)s_wire[1] != 0x8b ||
        (uint8_t)s_wire[2] != 8) {
        ESP_LOGW(TAG, "not a gzip stream");
        return -1;
    }
    uint8_t flg = (uint8_t)s_wire[3];
    int off = 10;
    if (flg & 0x04) { /* FEXTRA */
        if (off + 2 > len) {
            return -1;
        }
        off += 2 + ((uint8_t)s_wire[off] | ((uint8_t)s_wire[off + 1] << 8));
    }
    if (flg & 0x08) { /* FNAME */
        while (off < len && s_wire[off] != '\0') {
            off++;
        }
        off++;
    }
    if (flg & 0x10) { /* FCOMMENT */
        while (off < len && s_wire[off] != '\0') {
            off++;
        }
        off++;
    }
    if (flg & 0x02) { /* FHCRC */
        off += 2;
    }
    if (off >= len - 8) {
        return -1;
    }

    /* The trailing 8 bytes are CRC32 + ISIZE, not deflate data. No zlib-header
     * flag: gzip has no zlib wrapper. The output buffer is whole and big
     * enough, so tell tinfl it need not wrap. */
    const uint8_t *trailer = (const uint8_t *)s_wire + len - 4;
    uint32_t isize = (uint32_t)trailer[0] | (uint32_t)trailer[1] << 8 |
                     (uint32_t)trailer[2] << 16 | (uint32_t)trailer[3] << 24;
    if (isize == 0 || isize > RESPONSE_BUF_SIZE - 1) {
        ESP_LOGW(TAG, "inflated size %lu out of range", (unsigned long)isize);
        return -1;
    }
    s_buf = malloc(isize + 1);
    /* Heap, not stack: this ~11 KB struct would overflow the net task's
     * stack once mbedTLS has taken its share. */
    tinfl_decompressor *inflator = malloc(sizeof(*inflator));
    if (s_buf == NULL || inflator == NULL) {
        free(inflator);
        ESP_LOGW(TAG, "no memory to inflate %lu bytes", (unsigned long)isize);
        return -1;
    }
    size_t in_bytes = (size_t)(len - off - 8);
    size_t out_bytes = isize;
    tinfl_init(inflator);
    tinfl_status st = tinfl_decompress(inflator, (const mz_uint8 *)(s_wire + off), &in_bytes,
                                       (mz_uint8 *)s_buf, (mz_uint8 *)s_buf, &out_bytes,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(inflator);
    if (st != TINFL_STATUS_DONE || out_bytes != isize) {
        ESP_LOGW(TAG, "inflate failed (status %d, %d wire bytes)", (int)st, len);
        return -1;
    }
    return (int)out_bytes;
}

/* GET `path` into s_buf, NUL-terminated. The caller frees s_buf with
 * body_free() once it has parsed it; see http_get() below.
 *
 * HTTP/2 over a single long-lived session. The win over HTTP/1.1 is HPACK:
 * headers that repeat across requests -- and ours repeat almost exactly --
 * collapse to a couple of bytes each after the first exchange. On a 3-second
 * poll that was the dominant cost. */
static esp_err_t http_get_inner(const char *path)
{
    s_last_status = 0; /* or a failure here reports the previous request's */
    if (!s_h2_up) {
        struct sh2lib_config_t cfg = {
            .uri = BASE_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        if (sh2lib_connect(&cfg, &s_h2) != 0) {
            ESP_LOGW(TAG, "h2 connect failed");
            return ESP_FAIL;
        }
        s_h2_up = true;
        sh2lib_set_header_cb(h2_on_header);
    }

    /* Only now, with the connection up: see body_free(). */
    s_wire = malloc(WIRE_BUF_SIZE);
    if (s_wire == NULL) {
        ESP_LOGW(TAG, "no memory for the response");
        return ESP_ERR_NO_MEM;
    }

    s_wire_len = 0;
    s_stream_done = false;
    s_gzip = false;

    /* Built per request because the path changes; HPACK indexes the repeated
     * ones so the wire cost is far below these lengths after the first. */
    const nghttp2_nv nva[] = {
        SH2LIB_MAKE_NV(":method", "GET"),
        SH2LIB_MAKE_NV(":scheme", "https"),
        SH2LIB_MAKE_NV(":authority", s_h2.hostname),
        SH2LIB_MAKE_NV(":path", path),
        SH2LIB_MAKE_NV("accept-encoding", "gzip"),
        SH2LIB_MAKE_NV("cookie", s_cookie),
    };
    size_t nvlen = sizeof(nva) / sizeof(nva[0]);
    if (s_cookie[0] == '\0') {
        nvlen--; /* drop the cookie entry until we have been given one */
    }

    /* Returns the assigned stream ID on success -- a positive number -- and
     * negative on failure. Not zero. */
    if (sh2lib_do_get_with_nv(&s_h2, nva, nvlen, h2_on_data) < 0) {
        ESP_LOGW(TAG, "h2 submit failed");
        client_destroy();
        return ESP_FAIL;
    }

    /* nghttp2's recv callback is non-blocking, so poll rather than wait. The
     * deadline is what stops a half-dead session hanging the net task. */
    int64_t deadline = esp_timer_get_time() + (int64_t)HTTP_TIMEOUT_MS * 1000;
    while (!s_stream_done) {
        if (sh2lib_execute(&s_h2) != 0) {
            ESP_LOGW(TAG, "h2 session error");
            client_destroy();
            return ESP_FAIL;
        }
        if (s_stream_done) {
            break;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "h2 timeout for %s", path);
            client_destroy();
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_last_status != 200) {
        /* The cookie is bound to our public IP; a hotspot rebind gives 403 and
         * resending the dead one would fail forever. 429 is rate limiting, not
         * the cookie. */
        if (s_last_status >= 400 && s_last_status < 500 && s_last_status != 429) {
            s_cookie[0] = '\0';
        }
        ESP_LOGW(TAG, "HTTP %d for %s", s_last_status, path);
        return ESP_FAIL;
    }
    if (s_wire_len >= WIRE_BUF_SIZE) {
        ESP_LOGW(TAG, "response too large for wire buffer");
        return ESP_ERR_NO_MEM;
    }

    int body = s_wire_len;
    if (s_gzip) {
        body = gunzip(s_wire_len);
        if (body < 0) {
            return ESP_FAIL;
        }
    } else {
        s_buf = malloc((size_t)s_wire_len + 1);
        if (s_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(s_buf, s_wire, s_wire_len);
    }
    s_buf[body] = '\0';
    return ESP_OK;
}

/* On success s_buf holds the body until the caller's body_free(); on any
 * failure nothing is left allocated. The wire buffer never outlives the
 * request. */
static esp_err_t http_get(const char *path)
{
    body_free();
    esp_err_t err = http_get_inner(path);
    free(s_wire);
    s_wire = NULL;
    if (err != ESP_OK) {
        body_free();
    }
    return err;
}

/* http_get with retries. Transport errors and 5xx retry after a short
 * pause; 429 means the exchange asked us to slow down, so back off a full
 * second, growing per attempt. 4xx other than 429 will not improve by
 * retrying and fails straight through. */
static esp_err_t http_get_retry(const char *path)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            int delay_ms = (s_last_status == 429) ? 1000 * attempt : 300;
            ESP_LOGW(TAG, "retrying %s in %d ms (status %d)", path, delay_ms, s_last_status);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        err = http_get(path);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        bool retryable = s_last_status == 0 || s_last_status == 429 || s_last_status >= 500;
        if (!retryable) {
            return err;
        }
    }
    return err;
}

/* Parse {"code":"0","data":[...]} and return a detached `data` array. */
static cJSON *parse_data_array(void)
{
    cJSON *root = cJSON_Parse(s_buf);
    body_free(); /* parsed: the tree holds what is needed */
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse failed");
        return NULL;
    }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsString(code) || strcmp(code->valuestring, "0") != 0) {
        ESP_LOGW(TAG, "API error code %s", cJSON_IsString(code) ? code->valuestring : "?");
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *data = cJSON_DetachItemFromObject(root, "data");
    cJSON_Delete(root);
    return data;
}

esp_err_t okx_fetch_ticker(const char *inst_id, okx_ticker_t *out)
{
    char path[96];
    snprintf(path, sizeof(path), "/api/v5/market/ticker?instId=%s", inst_id);
    esp_err_t err = http_get_retry(path);
    if (err != ESP_OK) {
        /* OKX answers an unknown instId with HTTP 200 and an error code, but
         * a 400 means the same thing and is no reason to call it an outage. */
        return s_last_status == 400 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(s_buf);
    body_free();
    if (root == NULL) {
        return ESP_FAIL;
    }
    /* Only OKX's own "no such instrument" answer -- code 51001, or success
     * with nothing in it -- means the coin does not exist. Any other code
     * (rate limiting, "system busy") says nothing about the coin, and calling
     * it unknown would refuse a perfectly good one. */
    esp_err_t ret = ESP_FAIL;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *t = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "data"), 0);
    bool ok_code = cJSON_IsString(code) && strcmp(code->valuestring, "0") == 0;
    if (cJSON_IsString(code) && strcmp(code->valuestring, "51001") == 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (ok_code && t == NULL) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (ok_code) {
        cJSON *last = cJSON_GetObjectItem(t, "last");
        cJSON *open = cJSON_GetObjectItem(t, "open24h");
        if (cJSON_IsString(last) && cJSON_IsString(open)) {
            out->last = strtof(last->valuestring, NULL);
            out->open24h = strtof(open->valuestring, NULL);
            ret = ESP_OK;
        }
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t okx_fetch_candles(const char *inst_id, const char *bar, int max, int64_t before_ts,
                            int64_t after_ts, okx_candle_t *out, int *out_n,
                            int64_t *out_confirmed_ts)
{
    /* bar is always sent: OKX silently defaults it to 1m, which would quietly
     * poison the wrong series. */
    char path[192];
    int w = snprintf(path, sizeof(path), "/api/v5/market/candles?instId=%s&bar=%s&limit=%d",
                     inst_id, bar, max);
    if (before_ts != 0) {
        w += snprintf(path + w, sizeof(path) - w, "&before=%lld", (long long)before_ts);
    }
    if (after_ts != 0) {
        snprintf(path + w, sizeof(path) - w, "&after=%lld", (long long)after_ts);
    }
    ESP_RETURN_ON_ERROR(http_get_retry(path), TAG, "get candles");

    cJSON *data = parse_data_array();
    ESP_RETURN_ON_FALSE(data, ESP_FAIL, TAG, "bad candles response");

    /* Candles arrive newest first; store oldest first so index order is chart
     * order. Each is [ts, o, h, l, c, vol, volCcy, volCcyQuote, confirm]. */
    int n = cJSON_GetArraySize(data);
    if (n > max) {
        n = max;
    }
    esp_err_t ret = ESP_OK;
    int64_t confirmed = 0;

    for (int i = 0; i < n; i++) {
        cJSON *c = cJSON_GetArrayItem(data, n - 1 - i);
        cJSON *c_ts = cJSON_GetArrayItem(c, 0);
        cJSON *c_high = cJSON_GetArrayItem(c, 2);
        cJSON *c_low = cJSON_GetArrayItem(c, 3);
        cJSON *c_close = cJSON_GetArrayItem(c, 4);
        cJSON *c_conf = cJSON_GetArrayItem(c, 8);
        if (!cJSON_IsString(c_ts) || !cJSON_IsString(c_high) || !cJSON_IsString(c_low) ||
            !cJSON_IsString(c_close) || !cJSON_IsString(c_conf)) {
            /* Reject the whole batch rather than skipping the bad entry: a
             * short batch merged against timestamps would leave a hole that
             * nothing downstream can detect. */
            ESP_LOGW(TAG, "malformed candle %d/%d for %s %s", i, n, inst_id, bar);
            ret = ESP_FAIL;
            break;
        }
        okx_candle_t *o = &out[i];
        o->ts_ms = strtoll(c_ts->valuestring, NULL, 10);
        o->high = strtof(c_high->valuestring, NULL);
        o->low = strtof(c_low->valuestring, NULL);
        o->close = strtof(c_close->valuestring, NULL);
        if (c_conf->valuestring[0] == '1' && o->ts_ms > confirmed) {
            confirmed = o->ts_ms;
        }
    }
    cJSON_Delete(data);

    if (ret != ESP_OK) {
        return ret;
    }
    *out_n = n;
    *out_confirmed_ts = confirmed;
    return n > 0 ? ESP_OK : ESP_FAIL;
}
