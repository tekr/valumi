#include "market_core.h"

#include <string.h>

bool coin_plan(const char *const *old_ids, int n_old, const char *const *new_ids, int n_new,
               int *old_of_new, int *new_of_old)
{
    bool same = n_old == n_new;
    for (int j = 0; j < n_old; j++) {
        new_of_old[j] = -1;
    }
    for (int i = 0; i < n_new; i++) {
        old_of_new[i] = -1;
        for (int j = 0; j < n_old; j++) {
            if (strcmp(old_ids[j], new_ids[i]) == 0) {
                old_of_new[i] = j;
                new_of_old[j] = i;
                break;
            }
        }
        same = same && old_of_new[i] == i;
    }
    return same;
}

int coin_stale_after_s(int n_coins, int page_ms, int poll_s, int lead_s, int margin_s,
                       int floor_s)
{
    int s = ((n_coins - 1) * page_ms + 999) / 1000 + poll_s - lead_s + margin_s;
    return s < floor_s ? floor_s : s;
}

void series_merge(series_t *s, const okx_candle_t *in, int n)
{
    for (int k = 0; k < n; k++) {
        candle_t c = {(int32_t)(in[k].ts_ms / 60000), in[k].close, in[k].high, in[k].low};
        int slot = -1;
        /* From the newest end: the forming bar being updated is almost always
         * the last one. */
        for (int j = s->n - 1; j >= 0 && s->v[j].ts_min >= c.ts_min; j--) {
            if (s->v[j].ts_min == c.ts_min) {
                slot = j;
                break;
            }
        }
        if (slot >= 0) {
            s->v[slot] = c;
            continue;
        }
        if (s->n > 0 && c.ts_min < s->v[s->n - 1].ts_min) {
            continue; /* older than the window and not held */
        }
        if (s->n == s->cap) {
            memmove(s->v, s->v + 1, (s->cap - 1) * sizeof(*s->v));
            s->n--;
        }
        s->v[s->n++] = c;
    }
}
