#include "memwatch.h"

#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

void memwatch_note(const char *what, const char *detail)
{
    /* Only a new low by at least this much is logged, so a slow creep does
     * not flood the log. */
    static volatile size_t s_logged = SIZE_MAX;
    size_t low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    if (low >= MEMWATCH_WARN_BYTES || low + 512 > s_logged) {
        return;
    }
    s_logged = low;
    ESP_LOGW("memwatch", "free memory reached a low of %u bytes, by the end of %s %s "
             "(now %u free, largest block %u)",
             (unsigned)low, what, detail, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
