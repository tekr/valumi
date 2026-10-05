#include "wifi_policy.h"

wifi_action_t wifi_policy(const wifi_policy_in_t *in)
{
    if (!in->setup &&
        (in->n_nets == 0 || in->now_us - in->down_since_us >= in->fallback_us)) {
        return WIFI_ACT_ENTER_SETUP;
    }
    if (in->n_nets == 0) {
        return WIFI_ACT_WAIT; /* nothing to try: setup mode is all there is */
    }
    if (in->setup && in->hotspot_clients > 0) {
        bool due = in->last_look_us == 0 || in->now_us - in->last_look_us >= in->look_every_us;
        return due ? WIFI_ACT_LOOK : WIFI_ACT_WAIT;
    }
    return WIFI_ACT_SWEEP;
}
