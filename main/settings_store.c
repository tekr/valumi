#include "settings_store.h"

#include <stdlib.h>
#include <string.h>

#include "bootloader_random.h"
#include "cJSON.h"
#include "code.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings_json.h"

/* Defined by CMakeLists.txt when the file exists -- not __has_include, which
 * the build system cannot see appear. */
#if APP_HAVE_WIFI_SECRETS
#include "wifi_secrets.h"
#endif

static const char *TAG = "settings";

#define NVS_NS "ticker" /* from the old name; renaming it would lose saved settings */
/* The settings as JSON (settings_to_stored_json): firmware reads the fields
 * it knows, so settings survive updates that add or drop some. */
#define KEY_SETTINGS "settings_json"
#define KEY_NO_SEED "no_seed" /* set by a factory reset */
#define SAVE_LATER_US (3 * 1000000)

static SemaphoreHandle_t s_lock;
static settings_t s_cur;
static volatile uint32_t s_gen = 1;
static esp_timer_handle_t s_save_timer;

/* Caller holds s_lock. */
static esp_err_t persist_locked(const settings_t *s)
{
    cJSON *j = settings_to_stored_json(s);
    char *text = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    esp_err_t err = ESP_ERR_NO_MEM;
    nvs_handle_t h;
    if (text != NULL && (err = nvs_open(NVS_NS, NVS_READWRITE, &h)) == ESP_OK) {
        err = nvs_set_str(h, KEY_SETTINGS, text);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (text != NULL) {
        memset(text, 0, strlen(text)); /* holds passwords */
        free(text);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void save_timer_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    persist_locked(&s_cur);
    xSemaphoreGive(s_lock);
}

typedef enum {
    LOAD_NONE,       /* nothing saved: a first boot or a factory reset */
    LOAD_UNREADABLE, /* something saved that could not be read this boot */
    LOAD_OK,
} load_result_t;

/* Onto s_cur, which holds the defaults. */
static load_result_t load(nvs_handle_t h)
{
    size_t len = 0;
    if (nvs_get_str(h, KEY_SETTINGS, NULL, &len) != ESP_OK) {
        return LOAD_NONE;
    }
    char *text = malloc(len);
    cJSON *j = NULL;
    if (text != NULL && nvs_get_str(h, KEY_SETTINGS, text, &len) == ESP_OK) {
        j = cJSON_Parse(text);
        memset(text, 0, len);
    }
    free(text);
    if (!cJSON_IsObject(j)) {
        ESP_LOGW(TAG, "saved settings unreadable, running on defaults");
        cJSON_Delete(j);
        return LOAD_UNREADABLE;
    }
    settings_from_stored_json(&s_cur, j);
    cJSON_Delete(j);
    return LOAD_OK;
}

/* No usable settings: take the development seed, if there is one -- unless
 * this is the first boot after a factory reset, which has to land in setup
 * mode on every build, or setup mode could never be reached on the bench. */
static void seed(nvs_handle_t h)
{
    uint8_t no_seed = 0;
    if (nvs_get_u8(h, KEY_NO_SEED, &no_seed) == ESP_OK && no_seed) {
        nvs_erase_key(h, KEY_NO_SEED);
        nvs_commit(h);
        return;
    }
    settings_t unseeded = s_cur;
#ifdef APP_SEED_WIFI_NETWORKS
    static const struct {
        const char *ssid, *password;
    } nets[] = APP_SEED_WIFI_NETWORKS;
    for (size_t i = 0; i < sizeof(nets) / sizeof(nets[0]) && i < SET_MAX_NETS; i++) {
        strlcpy(s_cur.nets[i].ssid, nets[i].ssid, sizeof(s_cur.nets[i].ssid));
        strlcpy(s_cur.nets[i].password, nets[i].password, sizeof(s_cur.nets[i].password));
        s_cur.n_nets++;
    }
    ESP_LOGI(TAG, "seeded %d network(s) from wifi_secrets.h", s_cur.n_nets);
#endif
#ifdef APP_SEED_PANEL_PASSWORD
    _Static_assert(sizeof(APP_SEED_PANEL_PASSWORD) == SET_PANEL_PW_LEN + 1,
                   "APP_SEED_PANEL_PASSWORD must be 5 characters of A-Z and 2-9");
    strlcpy(s_cur.panel_pw, APP_SEED_PANEL_PASSWORD, sizeof(s_cur.panel_pw));
#endif
    /* A bad seed would make every later save fail validation. */
    char err[160];
    if (!settings_validate(&s_cur, err, sizeof(err))) {
        ESP_LOGE(TAG, "wifi_secrets.h seed rejected (%s); running unseeded", err);
        s_cur = unseeded;
    }
    memset(&unseeded, 0, sizeof(unseeded));
}

esp_err_t settings_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t targs = {.callback = save_timer_cb, .name = "settings_save"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_save_timer));

    /* A fresh password first: it stays unless one is saved or seeded. Wi-Fi is
     * not up yet, and without the radio esp_random() is only pseudo-random,
     * so borrow the bootloader's entropy source. */
    settings_defaults(&s_cur);
    bootloader_random_enable();
    code_random(s_cur.panel_pw, SET_PANEL_PW_LEN, esp_random);
    bootloader_random_disable();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s - running on defaults", esp_err_to_name(err));
        return err;
    }
    load_result_t loaded = load(h);
    if (loaded == LOAD_NONE) {
        seed(h);
    }
    nvs_close(h);
    /* Saved again so it carries the fields this firmware knows -- unless
     * it could not be read, when it may only be short of memory this boot,
     * and writing defaults over it would lose it for good. */
    if (loaded != LOAD_UNREADABLE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        persist_locked(&s_cur);
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "%s: %d network(s), %d coin(s)", loaded == LOAD_OK ? "loaded" : "defaults",
             s_cur.n_nets, s_cur.n_coins);
    return ESP_OK;
}

settings_t *settings_dup(void)
{
    settings_t *s = malloc(sizeof(*s));
    if (s != NULL) {
        settings_get(s);
    }
    return s;
}

void settings_free(settings_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
        free(s);
    }
}

void settings_flush(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (esp_timer_is_active(s_save_timer)) {
        esp_timer_stop(s_save_timer);
        persist_locked(&s_cur);
    }
    xSemaphoreGive(s_lock);
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

uint32_t settings_generation(void)
{
    return s_gen;
}

esp_err_t settings_update(const settings_t *s, settings_save_t when)
{
    char why[96];
    if (!settings_validate(s, why, sizeof(why))) {
        ESP_LOGW(TAG, "refused: %s", why);
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = memcmp(&s_cur, s, sizeof(*s)) != 0;
    if (when == SETTINGS_SAVE_NOW) {
        /* Saved before it takes effect, so a failed save changes nothing.
         * Writing @p s also covers a lazy save still pending. */
        if (changed || esp_timer_is_active(s_save_timer)) {
            err = persist_locked(s);
        }
        if (err == ESP_OK) {
            esp_timer_stop(s_save_timer);
        }
    } else if (changed) {
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, SAVE_LATER_US);
    }
    if (changed && err == ESP_OK) {
        s_cur = *s;
        s_gen++;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void settings_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_timer_stop(s_save_timer);
    /* The whole partition: settings, and anything the Wi-Fi driver kept. */
    nvs_flash_deinit();
    nvs_flash_erase();
    if (nvs_flash_init() == ESP_OK) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, KEY_NO_SEED, 1);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    esp_restart();
}
