#include "auth.h"

#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings_store.h"

static SemaphoreHandle_t s_lock;
static auth_state_t s_state;

static void lock(void)
{
    /* Created on first use, so no init call is needed whichever task gets
     * here first. */
    if (s_lock == NULL) {
        static StaticSemaphore_t buf;
        s_lock = xSemaphoreCreateMutexStatic(&buf);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void new_session_locked(char sid[AUTH_TOKEN_HEX + 1])
{
    uint8_t r[AUTH_TOKEN_BYTES];
    esp_fill_random(r, sizeof(r));
    auth_session_create(&s_state, r, esp_timer_get_time(), sid);
}

int auth_login_password(const char *password, char sid[AUTH_TOKEN_HEX + 1])
{
    lock();
    bool allowed = auth_login_allowed(&s_state, esp_timer_get_time());
    unlock();
    if (!allowed) {
        return -1;
    }

    settings_t *s = settings_dup();
    bool ok = s != NULL && auth_password_ok(password, s->panel_pw);
    settings_free(s);

    lock();
    auth_login_result(&s_state, ok, esp_timer_get_time());
    if (ok) {
        new_session_locked(sid);
    }
    unlock();
    return ok ? 1 : 0;
}

bool auth_login_qr(const char *token, char sid[AUTH_TOKEN_HEX + 1])
{
    lock();
    bool ok = auth_qr_redeem(&s_state, token, esp_timer_get_time());
    if (ok) {
        new_session_locked(sid);
    }
    unlock();
    return ok;
}

bool auth_check(const char *sid)
{
    lock();
    bool ok = auth_session_check(&s_state, sid, esp_timer_get_time());
    unlock();
    return ok;
}

void auth_logout(const char *sid)
{
    lock();
    auth_session_drop(&s_state, sid);
    unlock();
}

void auth_issue_qr(int screen_s, char token[AUTH_TOKEN_HEX + 1])
{
    uint8_t r[AUTH_TOKEN_BYTES];
    esp_fill_random(r, sizeof(r));
    lock();
    auth_qr_issue(&s_state, r, esp_timer_get_time(),
                  (int64_t)(screen_s + AUTH_QR_GRACE_S) * 1000000, token);
    unlock();
}
