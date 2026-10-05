#include "firmware.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

#define SETTLE_US (60 * 1000000LL)

void firmware_confirm(void)
{
    /* Only while on probation. Once an update is staged, the otadata entry
     * marking would touch is the NEW image's, and confirming that would
     * skip its probation entirely. */
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI("firmware", "new firmware confirmed");
    }
}

void firmware_confirm_when_settled(bool reachable, int64_t now_us)
{
    static bool done;
    static int64_t reachable_since;
    if (done) {
        return;
    }
    if (!reachable) {
        reachable_since = 0;
        return;
    }
    if (reachable_since == 0) {
        reachable_since = now_us;
    }
    if (now_us - reachable_since >= SETTLE_US) {
        done = true;
        firmware_confirm();
    }
}
