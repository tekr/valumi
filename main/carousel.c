#include "carousel.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static int s_page;
static uint32_t s_turn;
static int64_t s_due_us;
/* Both set from the settings and changeable at any time. */
static int s_count = 1;
static int64_t s_dwell_us;


static int wrap(int p)
{
    p %= s_count;
    return p < 0 ? p + s_count : p;
}

void carousel_init(int count, int dwell_s)
{
    s_lock = xSemaphoreCreateMutex();
    s_count = count > 0 ? count : 1;
    s_dwell_us = (int64_t)dwell_s * 1000000;
    s_page = 0;
    s_due_us = esp_timer_get_time() + s_dwell_us;
}

void carousel_configure(int count, int dwell_s, const int *new_of_old)
{
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (new_of_old) {
        s_page = new_of_old[s_page] >= 0 ? new_of_old[s_page] : 0;
    }
    s_count = count > 0 ? count : 1;
    int64_t dwell_us = (int64_t)dwell_s * 1000000;
    if (dwell_us != s_dwell_us) {
        /* A shorter dwell takes effect now rather than after the old, longer
         * deadline; a longer one extends the page already showing. */
        s_due_us = now + dwell_us;
        s_dwell_us = dwell_us;
    }
    s_page = wrap(s_page);
    xSemaphoreGive(s_lock);
}

int carousel_tick(bool held)
{
    int64_t now = esp_timer_get_time();
    int moved = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (held) {
        /* Push the deadline along rather than remembering that we paused.
         * Restarting the full dwell on release then falls out of it, and the
         * net task sees a pre-fetch window that simply never arrives while the
         * finger is down -- which is exactly right, since nothing is about to
         * change. */
        s_due_us = now + s_dwell_us;
    } else if (now >= s_due_us) {
        s_due_us = now + s_dwell_us;
        if (s_count > 1) {
            s_page = wrap(s_page + 1);
            s_turn++;
            moved = 1;
        }
    }
    xSemaphoreGive(s_lock);
    return moved;
}

int carousel_jump(int dir)
{
    if (dir == 0) {
        return 0;
    }
    int64_t now = esp_timer_get_time();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_page = wrap(s_page + dir);
    s_turn++;
    s_due_us = now + s_dwell_us;
    xSemaphoreGive(s_lock);
    return dir > 0 ? 1 : -1;
}

int carousel_page(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int p = s_page;
    xSemaphoreGive(s_lock);
    return p;
}

void carousel_get(carousel_view_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->turn = s_turn;
    out->page = s_page;
    out->next_page = wrap(s_page + 1);
    out->due_us = s_due_us;
    xSemaphoreGive(s_lock);
}
