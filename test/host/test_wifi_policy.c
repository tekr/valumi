#include "test.h"
#include "wifi_policy.h"

#define S 1000000LL

static wifi_policy_in_t base(void)
{
    wifi_policy_in_t in = {
        .n_nets = 2,
        .setup = false,
        .hotspot_clients = 0,
        .now_us = 10 * S,
        .down_since_us = 0,
        .last_look_us = 0,
        .fallback_us = 120 * S,
        .look_every_us = 60 * S,
    };
    return in;
}

static void test_no_networks_goes_straight_to_setup(void)
{
    wifi_policy_in_t in = base();
    in.n_nets = 0;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_ENTER_SETUP);
    in.setup = true;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_WAIT);
}

static void test_fallback_after_two_minutes(void)
{
    wifi_policy_in_t in = base();
    in.now_us = 119 * S;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_SWEEP); /* keep trying */
    in.now_us = 120 * S;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_ENTER_SETUP);
    /* Measured from when the network was LOST, not from boot. */
    in.down_since_us = 100 * S;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_SWEEP);
}

static void test_setup_keeps_retrying_when_nobody_is_on_it(void)
{
    wifi_policy_in_t in = base();
    in.setup = true;
    in.now_us = 1000 * S;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_SWEEP);
}

static void test_setup_with_a_phone_looks_but_does_not_sweep(void)
{
    wifi_policy_in_t in = base();
    in.setup = true;
    in.hotspot_clients = 1;
    in.now_us = 1000 * S;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_LOOK); /* never looked */
    in.last_look_us = 980 * S;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_WAIT); /* looked 20 s ago */
    in.last_look_us = 940 * S;
    CHECK_EQ_INT(wifi_policy(&in), WIFI_ACT_LOOK); /* 60 s: look again */
}

int main(void)
{
    RUN(test_no_networks_goes_straight_to_setup);
    RUN(test_fallback_after_two_minutes);
    RUN(test_setup_keeps_retrying_when_nobody_is_on_it);
    RUN(test_setup_with_a_phone_looks_but_does_not_sweep);
    TEST_MAIN_END("wifi_policy");
}
