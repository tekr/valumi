#include "board_orientation.h"
#include "board_imu.h"

#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "board_orientation";

/* How often to ask the IMU. Orientation changes are a human picking a board up
 * and turning it round; 10 Hz is far more than enough, and it keeps the I2C
 * traffic out of the way of a render loop that may be running at 50 Hz. */
#define SAMPLE_INTERVAL_US 100000

/* Gravity along the screen's vertical axis, in g, before a flip is considered.
 * 0.35 g is about 20 degrees off flat. Below that the board is lying face up
 * on a desk, where "which way is up" genuinely has no answer -- so the
 * deadband is not merely debouncing, it is the honest range where the question
 * is unanswerable, and holding the last answer is the right thing to do. */
#define FLIP_THRESHOLD_G 0.35f

/* And it has to stay that way for this long. A board being carried swings
 * through both orientations; without the dwell the screen would flip during
 * the journey and flip back on arrival. */
#define FLIP_DWELL_US 700000

static bool s_available;
static bool s_flipped;
static int64_t s_candidate_since_us; /* 0 = the current reading agrees with s_flipped */
static bool s_candidate;
static int64_t s_last_sample_us;

esp_err_t board_orientation_init(void)
{
    board_imu_init();

    /* The simulated source sweeps gravity right round the compass every 13
     * seconds. Driving a screen flip from that would be a spinning display and
     * a puzzled owner, so anything but a real part counts as no sensor. */
    if (board_imu_source() != BOARD_IMU_SOURCE_QMI8658) {
        ESP_LOGI(TAG, "no accelerometer (%s) - orientation locked", board_imu_source_name());
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_available = true;
    ESP_LOGI(TAG, "orientation follows the QMI8658");
    return ESP_OK;
}

bool board_orientation_available(void)
{
    return s_available;
}

/* Component of gravity DOWN the screen -- along its SHORT axis -- positive
 * when the picture is the right way up in the unflipped orientation.
 *
 * The short axis is the one to read, and getting this wrong is subtle. Turning
 * the board over in the plane of the glass negates both screen axes, so either
 * would flip sign in principle. But a landscape screen held normally has
 * gravity running straight down its short axis and essentially nothing along
 * the long one, so reading the long axis asks the question in the one
 * direction where the answer is always near zero: a deadband hit at best, and
 * at worst a screen that turns over only when you hold it in PORTRAIT and
 * rotate it there. That is exactly how the wrong choice presented.
 *
 * Established on the part, each line by watching the screen rather than by
 * reading a datasheet:
 *
 *   flat on a desk, face up   z goes to -1     -> z is the screen normal
 *   turned in PORTRAIT        y is what moves  -> y is the long axis
 *   turned in LANDSCAPE       x is what moves  -> x is the short axis
 *
 * and the sign was fixed by the only test that can fix it: putting the flip
 * live and turning the board over until the picture stayed upright. x reads
 * POSITIVE when the board is the right way up.
 *
 * If the screen ever settles upside down, this sign is the whole of the fix.
 * Remeasure on the hardware; do not reason about it. An earlier
 * attempt to derive it from a held-still reading got the sign wrong twice,
 * because "hold it upright" is ambiguous until the screen itself says which
 * way up it thinks it is. */
static float gravity_down_screen(const board_accel_t *a)
{
    return a->x;
}

bool board_orientation_flipped(void)
{
    if (!s_available) {
        return false;
    }

    int64_t now = esp_timer_get_time();
    if (now - s_last_sample_us < SAMPLE_INTERVAL_US) {
        return s_flipped;
    }
    s_last_sample_us = now;

    board_accel_t a;
    if (!board_imu_read(&a)) {
        return s_flipped; /* no new sample: hold, never guess */
    }

    float g = gravity_down_screen(&a);
    if (fabsf(g) < FLIP_THRESHOLD_G) {
        /* Too flat to tell. Abandon any candidate rather than let it age into
         * a flip on evidence that stopped arriving. */
        s_candidate_since_us = 0;
        return s_flipped;
    }

    /* Down the screen is positive when upright, so a negative reading means the
     * board has been turned over. */
    bool want = g < 0.0f;
    if (want == s_flipped) {
        s_candidate_since_us = 0;
        return s_flipped;
    }

    if (s_candidate_since_us == 0 || s_candidate != want) {
        s_candidate = want;
        s_candidate_since_us = now;
        return s_flipped;
    }
    if (now - s_candidate_since_us >= FLIP_DWELL_US) {
        s_flipped = want;
        s_candidate_since_us = 0;
    }
    return s_flipped;
}