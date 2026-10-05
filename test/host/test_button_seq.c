#include "button_seq.h"
#include "test.h"

#define MS 1000LL

/* Hold for @p hold_ms sampling every 10 ms, then release. Returns a string
 * of the events in order: S L I R (short, long, login, reset). */
static void run_hold(int64_t hold_ms, char *out)
{
    bseq_t b;
    bseq_init(&b);
    int64_t t = 1000 * MS;
    int k = 0;
    /* idle a moment first */
    for (int i = 0; i < 5; i++, t += 10 * MS) {
        bseq_update(&b, false, t);
    }
    int64_t press = t;
    for (; t < press + hold_ms * MS; t += 10 * MS) {
        bseq_event_t e = bseq_update(&b, true, t);
        if (e) {
            out[k++] = " SLIR"[e];
        }
    }
    for (int i = 0; i < 10; i++, t += 10 * MS) {
        bseq_event_t e = bseq_update(&b, false, t);
        if (e) {
            out[k++] = " SLIR"[e];
        }
    }
    out[k] = '\0';
}

static void test_timeline(void)
{
    char ev[16];
    run_hold(200, ev);
    CHECK_EQ_STR(ev, "S");
    run_hold(990, ev);
    CHECK_EQ_STR(ev, "S");
    run_hold(1500, ev);
    CHECK_EQ_STR(ev, "L"); /* range, nothing on release */
    run_hold(4900, ev);
    CHECK_EQ_STR(ev, "L"); /* let go just before login: range only */
    run_hold(5100, ev);
    CHECK_EQ_STR(ev, "LI"); /* range on the way, then login on release */
    run_hold(9900, ev);
    CHECK_EQ_STR(ev, "LI");
    run_hold(10100, ev);
    CHECK_EQ_STR(ev, "LR"); /* reset fires held; release says nothing */
    run_hold(30000, ev);
    CHECK_EQ_STR(ev, "LR"); /* and fires only once */
}

static void test_long_fires_while_held(void)
{
    bseq_t b;
    bseq_init(&b);
    CHECK_EQ_INT(bseq_update(&b, true, 0), BSEQ_NONE);
    CHECK_EQ_INT(bseq_update(&b, true, 999 * MS), BSEQ_NONE);
    CHECK_EQ_INT(bseq_update(&b, true, 1000 * MS), BSEQ_LONG); /* at 1 s, not on release */
    CHECK_EQ_INT(bseq_update(&b, true, 1010 * MS), BSEQ_NONE);
    CHECK_EQ_INT(bseq_update(&b, true, 9999 * MS), BSEQ_NONE);
    CHECK_EQ_INT(bseq_update(&b, true, 10000 * MS), BSEQ_RESET); /* at 10 s, still held */
}

static void test_debounce(void)
{
    bseq_t b;
    bseq_init(&b);
    int64_t t = 0;
    CHECK_EQ_INT(bseq_update(&b, true, t), BSEQ_NONE);
    /* Contact bounce inside 30 ms is ignored: no spurious SHORT. */
    CHECK_EQ_INT(bseq_update(&b, false, t + 5 * MS), BSEQ_NONE);
    CHECK_EQ_INT(bseq_update(&b, true, t + 10 * MS), BSEQ_NONE);
    CHECK_EQ_INT(bseq_update(&b, false, t + 20 * MS), BSEQ_NONE);
    /* The real release. */
    CHECK_EQ_INT(bseq_update(&b, false, t + 200 * MS), BSEQ_SHORT);
}

static void test_hints(void)
{
    bseq_t b;
    bseq_init(&b);
    int s;
    bseq_update(&b, true, 0);
    CHECK_EQ_INT(bseq_hint(&b, 1500 * MS, &s), BSEQ_HINT_NONE);
    CHECK_EQ_INT(bseq_hint(&b, 2000 * MS, &s), BSEQ_HINT_LOGIN);
    CHECK_EQ_INT(s, 3); /* "3, 2, 1" */
    CHECK_EQ_INT(bseq_hint(&b, 2001 * MS, &s), BSEQ_HINT_LOGIN);
    CHECK_EQ_INT(s, 3);
    CHECK_EQ_INT(bseq_hint(&b, 4001 * MS, &s), BSEQ_HINT_LOGIN);
    CHECK_EQ_INT(s, 1);
    CHECK_EQ_INT(bseq_hint(&b, 5000 * MS, &s), BSEQ_HINT_RESET);
    CHECK_EQ_INT(s, 5);
    CHECK_EQ_INT(bseq_hint(&b, 9500 * MS, &s), BSEQ_HINT_RESET);
    CHECK_EQ_INT(s, 1);
    CHECK_EQ_INT(bseq_update(&b, true, 1000 * MS), BSEQ_LONG);
    CHECK_EQ_INT(bseq_update(&b, true, 10000 * MS), BSEQ_RESET);
    CHECK_EQ_INT(bseq_hint(&b, 10000 * MS, &s), BSEQ_HINT_NONE);
    /* Not pressed: no hint. */
    bseq_init(&b);
    CHECK_EQ_INT(bseq_hint(&b, 5000 * MS, &s), BSEQ_HINT_NONE);
}

static void test_reset_countdown(void)
{
    bseq_t b;
    bseq_init(&b);
    int s;
    bseq_reset_countdown(&b, 0, &s);
    CHECK_EQ_INT(s, 0); /* not held */
    bseq_update(&b, true, 0);
    bseq_reset_countdown(&b, 2000 * MS, &s);
    CHECK_EQ_INT(s, 8); /* setup mode counts straight to the reset */
    bseq_reset_countdown(&b, 9001 * MS, &s);
    CHECK_EQ_INT(s, 1);
    bseq_update(&b, true, 1000 * MS);
    bseq_update(&b, true, 10000 * MS); /* reset fires */
    bseq_reset_countdown(&b, 10000 * MS, &s);
    CHECK_EQ_INT(s, 0);
}

static void test_next_press_after_reset_is_fresh(void)
{
    bseq_t b;
    bseq_init(&b);
    bseq_update(&b, true, 0);
    bseq_update(&b, true, 1000 * MS);
    CHECK_EQ_INT(bseq_update(&b, true, 10000 * MS), BSEQ_RESET);
    CHECK_EQ_INT(bseq_update(&b, false, 10100 * MS), BSEQ_NONE);
    bseq_update(&b, true, 11000 * MS);
    CHECK_EQ_INT(bseq_update(&b, false, 11100 * MS), BSEQ_SHORT);
}

int main(void)
{
    RUN(test_timeline);
    RUN(test_long_fires_while_held);
    RUN(test_debounce);
    RUN(test_hints);
    RUN(test_reset_countdown);
    RUN(test_next_press_after_reset_is_fresh);
    TEST_MAIN_END("button_seq");
}
