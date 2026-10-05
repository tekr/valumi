#include "carousel.h"
#include "test.h"

extern int64_t g_fake_now_us;
#define S 1000000LL

static void test_dwell_and_wrap(void)
{
    g_fake_now_us = 0;
    carousel_init(3, 8);
    CHECK_EQ_INT(carousel_page(), 0);
    g_fake_now_us = 8 * S - 1;
    CHECK_EQ_INT(carousel_tick(false), 0);
    g_fake_now_us = 8 * S;
    CHECK_EQ_INT(carousel_tick(false), 1);
    CHECK_EQ_INT(carousel_page(), 1);
    g_fake_now_us = 16 * S;
    carousel_tick(false);
    g_fake_now_us = 24 * S;
    carousel_tick(false);
    CHECK_EQ_INT(carousel_page(), 0); /* wrapped */
}

static void test_hold_and_jump(void)
{
    g_fake_now_us = 0;
    carousel_init(4, 8);
    g_fake_now_us = 7 * S;
    CHECK_EQ_INT(carousel_tick(true), 0); /* finger down */
    g_fake_now_us = 30 * S;
    CHECK_EQ_INT(carousel_tick(true), 0); /* still held, still page 0 */
    g_fake_now_us = 37 * S;
    CHECK_EQ_INT(carousel_tick(false), 0); /* full dwell restarts on lift */
    g_fake_now_us = 38 * S;
    CHECK_EQ_INT(carousel_tick(false), 1);

    CHECK_EQ_INT(carousel_jump(-1), -1);
    CHECK_EQ_INT(carousel_page(), 0);
    CHECK_EQ_INT(carousel_jump(-1), -1);
    CHECK_EQ_INT(carousel_page(), 3);
    carousel_view_t v;
    carousel_get(&v);
    CHECK_EQ_INT(v.next_page, 0);
    CHECK_EQ_INT(v.due_us, 38 * S + 8 * S); /* a jump restarts the dwell */
}

static void test_configure_live(void)
{
    g_fake_now_us = 0;
    carousel_init(4, 8);
    carousel_jump(1);
    carousel_jump(1);
    carousel_jump(1); /* page 3 */
    g_fake_now_us = 2 * S;

    /* A new list moves the coin on screen to its new index. */
    carousel_configure(2, 8, (const int[]){-1, -1, 0, 1}); /* page 3 is now 1 */
    CHECK_EQ_INT(carousel_page(), 1);
    /* An unchanged list keeps the page, clamped into range. */
    carousel_configure(4, 8, NULL);
    carousel_jump(1);
    carousel_jump(1); /* page 3 */
    carousel_configure(2, 8, NULL);
    CHECK_EQ_INT(carousel_page(), 1); /* 3 wraps to 1 */

    /* Unchanged dwell: the running deadline is left alone. */
    carousel_view_t before, after;
    carousel_get(&before);
    g_fake_now_us = 5 * S;
    carousel_configure(2, 8, NULL);
    carousel_get(&after);
    CHECK_EQ_INT(after.due_us, before.due_us);

    /* A shorter dwell takes effect from now, not after the old deadline. */
    carousel_configure(2, 3, NULL);
    carousel_get(&after);
    CHECK_EQ_INT(after.due_us, 5 * S + 3 * S);
    g_fake_now_us = 8 * S;
    CHECK_EQ_INT(carousel_tick(false), 1);
    carousel_get(&after);
    CHECK_EQ_INT(after.due_us, 11 * S);

    /* A single coin never "moves" to itself visibly but still wraps safely. */
    carousel_configure(1, 3, NULL);
    CHECK_EQ_INT(carousel_page(), 0);
    carousel_view_t v;
    carousel_get(&v);
    CHECK_EQ_INT(v.next_page, 0);
}

/* The list is planned while coin 1 (BTC) shows, but its time runs out before
 * the plan is applied and coin 2 (ETH) comes on. The remap must follow ETH,
 * not put BTC back. */
static void test_remap_follows_the_current_page(void)
{
    g_fake_now_us = 0;
    carousel_init(4, 8);
    carousel_jump(1); /* BTC, index 1 */
    int new_of_old[] = {-1, 2, 0, 1}; /* OKB removed; ETH first, SOL, BTC */
    g_fake_now_us = 8 * S;
    CHECK_EQ_INT(carousel_tick(false), 1); /* ETH, index 2, comes on */
    carousel_configure(3, 8, new_of_old);
    CHECK_EQ_INT(carousel_page(), 0); /* still ETH, at its new index */

    /* The coin on screen removed: start from the first. */
    carousel_configure(2, 8, (const int[]){-1, 0, 1});
    CHECK_EQ_INT(carousel_page(), 0);
}

/* Every page change bumps the turn, so the market prefetches once per page
 * even when the deadline moves every frame. One page never "changes". */
static void test_turns_and_zero_dwell(void)
{
    g_fake_now_us = 0;
    carousel_init(3, 0);
    carousel_view_t v;
    carousel_get(&v);
    uint32_t t0 = v.turn;
    g_fake_now_us = 1;
    CHECK_EQ_INT(carousel_tick(true), 0); /* held while the last page slides in */
    CHECK_EQ_INT(carousel_tick(false), 1); /* arrived: no dwell, straight on */
    carousel_get(&v);
    CHECK_EQ_INT(v.turn, t0 + 1);
    carousel_jump(-1);
    carousel_get(&v);
    CHECK_EQ_INT(v.turn, t0 + 2);

    carousel_configure(1, 0, NULL);
    carousel_get(&v);
    t0 = v.turn;
    g_fake_now_us = 2;
    CHECK_EQ_INT(carousel_tick(false), 0);
    carousel_get(&v);
    CHECK_EQ_INT(v.turn, t0);
}

int main(void)
{
    RUN(test_dwell_and_wrap);
    RUN(test_hold_and_jump);
    RUN(test_configure_live);
    RUN(test_remap_follows_the_current_page);
    RUN(test_turns_and_zero_dwell);
    TEST_MAIN_END("carousel");
}
