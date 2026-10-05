#include "button_seq.h"

#include <string.h>

void bseq_init(bseq_t *b)
{
    memset(b, 0, sizeof(*b));
    /* No edge yet, so the first real one is never inside a debounce window. */
    b->last_edge_us = INT64_MIN / 2;
}

bseq_event_t bseq_update(bseq_t *b, bool raw_down, int64_t now_us)
{
    if (raw_down != b->down && now_us - b->last_edge_us >= BSEQ_DEBOUNCE_US) {
        b->last_edge_us = now_us;
        b->down = raw_down;
        if (raw_down) {
            b->down_at_us = now_us;
            b->long_fired = false;
            b->reset_fired = false;
            return BSEQ_NONE;
        }
        int64_t held = now_us - b->down_at_us;
        if (b->reset_fired) {
            return BSEQ_NONE; /* the hold was consumed by the reset */
        }
        if (!b->long_fired) {
            return BSEQ_SHORT;
        }
        if (held >= BSEQ_LOGIN_US) {
            return BSEQ_LOGIN;
        }
        return BSEQ_NONE; /* 1-5 s: the LONG already answered it */
    }

    if (!b->down) {
        return BSEQ_NONE;
    }
    int64_t held = now_us - b->down_at_us;
    if (!b->long_fired && held >= BSEQ_LONG_US) {
        b->long_fired = true;
        return BSEQ_LONG;
    }
    if (!b->reset_fired && held >= BSEQ_RESET_US) {
        b->reset_fired = true;
        return BSEQ_RESET;
    }
    return BSEQ_NONE;
}

static int ceil_secs(int64_t us)
{
    return (int)((us + 999999) / 1000000);
}

bseq_hint_t bseq_hint(const bseq_t *b, int64_t now_us, int *secs_left)
{
    *secs_left = 0;
    if (!b->down || b->reset_fired) {
        return BSEQ_HINT_NONE;
    }
    int64_t held = now_us - b->down_at_us;
    if (held < BSEQ_HINT_US) {
        return BSEQ_HINT_NONE;
    }
    if (held < BSEQ_LOGIN_US) {
        *secs_left = ceil_secs(BSEQ_LOGIN_US - held);
        return BSEQ_HINT_LOGIN;
    }
    *secs_left = ceil_secs(BSEQ_RESET_US - held);
    return BSEQ_HINT_RESET;
}

void bseq_reset_countdown(const bseq_t *b, int64_t now_us, int *secs_left)
{
    *secs_left = b->down && !b->reset_fired ? ceil_secs(BSEQ_RESET_US - (now_us - b->down_at_us)) : 0;
}
