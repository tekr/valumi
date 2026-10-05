/*
 * What the Wi-Fi manager should do next while it has no network. Pure, so
 * the setup-mode rules -- the 2-minute fallback, standing still while a phone
 * is on the hotspot, still looking now and then -- are tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_ACT_WAIT,        /* nothing to do this second */
    WIFI_ACT_ENTER_SETUP, /* bring the hotspot up, then ask again */
    WIFI_ACT_SWEEP,       /* try the saved networks in order */
    WIFI_ACT_LOOK,        /* someone is on the hotspot: scan for a saved network,
                           * and join only if one is actually there */
} wifi_action_t;

typedef struct {
    int n_nets;              /* saved networks */
    bool setup;              /* hotspot up */
    int hotspot_clients;
    int64_t now_us;
    int64_t down_since_us;   /* when the last network was lost (or boot) */
    int64_t last_look_us;    /* last WIFI_ACT_LOOK, 0 = never */
    int64_t fallback_us;     /* offline this long -> setup mode */
    int64_t look_every_us;   /* with clients, look this often */
} wifi_policy_in_t;

wifi_action_t wifi_policy(const wifi_policy_in_t *in);

#ifdef __cplusplus
}
#endif
