/*
 * What one button means, by how long it is held.
 *
 *   released before 1 s      SHORT  -- brightness
 *   reaches 1 s              LONG   -- chart range, fired while still held
 *   released between 5-10 s  LOGIN  -- login screen
 *   reaches 10 s             RESET  -- factory reset, fired while still held
 *
 * LONG fires the moment 1 s is reached, so the everyday hold feels instant.
 * The price is accepted and deliberate: any hold long enough for LOGIN or
 * RESET has already changed the chart range once on the way.
 *
 * Pure: fed the raw level and the time, so the whole timeline is testable on
 * the host. Debounce matches board_button.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSEQ_DEBOUNCE_US 30000
#define BSEQ_LONG_US 1000000
#define BSEQ_HINT_US 2000000   /* countdown to the login screen starts here */
#define BSEQ_LOGIN_US 5000000
#define BSEQ_RESET_US 10000000

typedef enum {
    BSEQ_NONE,
    BSEQ_SHORT,
    BSEQ_LONG,
    BSEQ_LOGIN,
    BSEQ_RESET,
} bseq_event_t;

typedef enum {
    BSEQ_HINT_NONE,
    BSEQ_HINT_LOGIN, /* 2-5 s: "keep holding for login details" */
    BSEQ_HINT_RESET, /* 5-10 s: "release now for login, keep holding to reset" */
} bseq_hint_t;

typedef struct {
    bool down;          /* debounced level */
    int64_t last_edge_us;
    int64_t down_at_us;
    bool long_fired;
    bool reset_fired;
} bseq_t;

void bseq_init(bseq_t *b);

/** Feed one sample. At most one event per call. */
bseq_event_t bseq_update(bseq_t *b, bool raw_down, int64_t now_us);

/**
 * @brief What the screen should be saying about the hold in progress.
 * @param secs_left whole seconds until the next threshold, rounded up.
 */
bseq_hint_t bseq_hint(const bseq_t *b, int64_t now_us, int *secs_left);

/** Whole seconds, rounded up, until the reset fires. */
void bseq_reset_countdown(const bseq_t *b, int64_t now_us, int *secs_left);

#ifdef __cplusplus
}
#endif
