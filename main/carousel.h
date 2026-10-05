/*
 * Which coin is on screen, and when it changes.
 *
 * This used to be a pure function of the wall clock, which both tasks
 * evaluated independently -- elegant, and impossible once a finger can move
 * the carousel. The page is now state, owned here, and both tasks ask for it:
 * the render task drives it, the net task reads it to decide what to poll and
 * when to pre-fetch the incoming coin.
 *
 * Two things move the page:
 *   - the dwell timer expiring, which advances by one
 *   - a swipe, which jumps and restarts the dwell from the beginning, because
 *     a coin you asked for should get its full time on screen
 *
 * And one thing stops it: a finger resting on the glass. The dwell is pushed
 * forward for as long as the touch lasts, so it neither expires under the
 * finger nor fires the instant it lifts.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t turn;  /* bumped on every page change */
    int page;       /* the coin currently selected */
    int next_page;  /* the one the dwell timer will land on */
    int64_t due_us; /* when that happens, on the esp_timer clock */
} carousel_view_t;

void carousel_init(int count, int dwell_s);

/**
 * @brief Change the number of pages and the dwell, live.
 *
 * @param new_of_old for a new coin list: each old page's index in it, or -1
 *             if that coin is gone (the carousel then starts from the first).
 *             NULL when the list is unchanged. Applied to whatever page is
 *             current at the time, under the carousel's lock: a page read
 *             earlier may already have moved on, and remapping that would
 *             put the outgoing coin back on screen.
 *
 * A changed dwell restarts the current page's time; an unchanged one leaves
 * it running, so saving an unrelated setting does not visibly reset anything.
 */
void carousel_configure(int count, int dwell_s, const int *new_of_old);

/**
 * @brief Advance the carousel if its dwell has expired.
 *
 * Call once per rendered frame.
 *
 * @param held true while a finger is on the screen: the dwell is held off and
 *             restarts in full when the finger lifts.
 * @return +1 or -1 if the page changed this call, 0 otherwise. The sign is the
 *         direction to animate.
 */
int carousel_tick(bool held);

/**
 * @brief Move by @p dir pages now, and restart the dwell.
 *
 * @return the same value @p dir would produce as a direction, or 0 if the jump
 *         was ignored.
 */
int carousel_jump(int dir);

int carousel_page(void);

/** Consistent snapshot; the two fields must agree or the net task pre-fetches
 * the wrong coin across a page change. */
void carousel_get(carousel_view_t *out);

#ifdef __cplusplus
}
#endif
