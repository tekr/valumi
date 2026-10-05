/*
 * One frame of a change from one coin's page to the next: how the two pages
 * move and fade, for every transition the owner can choose. Pure drawing --
 * main owns the clock and decides when a change starts and ends.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int move;      /* SET_MOVE_* */
    bool vertical; /* pages change up and down rather than sideways */
    int style;     /* SET_STYLE_*: how slide and cascade fade */
    int fade;      /* how far a page dims, percent */
} page_transition_t;

/**
 * @brief Draw the frame @p p of the way (0..1) from @p from to @p to.
 *
 * Clears the frame first. @p dir is +1 when the new page comes from the
 * right (or, vertically, from below) and -1 for the opposite way, which is
 * the same picture mirrored.
 */
void page_transition_render(uint16_t *fb, const page_transition_t *t, const ui_coin_t *from,
                            const ui_coin_t *to, float p, int dir);

#ifdef __cplusplus
}
#endif
