#include "chart_path.h"

#include <math.h>
#include <stdbool.h>

#include "app_config.h"

#define MAX_POINTS (2 * APP_CHART_MAX + 1)

int chart_extremes(const float *close, const float *high, const float *low, int n,
                   chart_pt_t *out)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        float before = i > 0 ? close[i - 1] : (high[i] + low[i]) / 2.0f;
        bool rising = close[i] >= before;
        out[k++] = (chart_pt_t){i + 0.25f, rising ? low[i] : high[i]};
        out[k++] = (chart_pt_t){i + 0.75f, rising ? high[i] : low[i]};
    }
    out[k++] = (chart_pt_t){(float)n, close[n - 1]};
    return k;
}

int chart_swings(const chart_pt_t *p, int n, float min_frac, chart_pt_t *out)
{
    float lo = p[0].y, hi = p[0].y;
    for (int k = 1; k < n; k++) {
        lo = fminf(lo, p[k].y);
        hi = fmaxf(hi, p[k].y);
    }
    float min_swing = min_frac * (hi - lo);

    int m = 0;
    out[m++] = p[0];
    /* Until the first swing is clear, both extremes so far are candidates;
     * whichever came first is a pivot once they are far enough apart. Then
     * dir says which kind of extreme is being followed. */
    int dir = 0;
    chart_pt_t top = p[0], bottom = p[0];
    for (int k = 1; k < n; k++) {
        if (dir == 0) {
            top = p[k].y > top.y ? p[k] : top;
            bottom = p[k].y < bottom.y ? p[k] : bottom;
            if (top.y - bottom.y >= min_swing && top.y > bottom.y) {
                bool rising = bottom.x < top.x;
                chart_pt_t first = rising ? bottom : top;
                if (first.x != p[0].x) {
                    out[m++] = first;
                }
                dir = rising ? 1 : -1;
            }
        } else if (dir > 0) {
            if (p[k].y > top.y) {
                top = p[k];
            } else if (top.y - p[k].y >= min_swing) {
                out[m++] = top;
                bottom = p[k];
                dir = -1;
            }
        } else {
            if (p[k].y < bottom.y) {
                bottom = p[k];
            } else if (p[k].y - bottom.y >= min_swing) {
                out[m++] = bottom;
                top = p[k];
                dir = 1;
            }
        }
    }
    /* The swing still open, then the last point. */
    chart_pt_t open = dir > 0 ? top : bottom;
    if (dir != 0 && open.x != p[n - 1].x) {
        out[m++] = open;
    }
    out[m++] = p[n - 1];
    return m;
}

void chart_columns(const chart_pt_t *p, int n, int w, float *out)
{
    float col[MAX_POINTS], y[MAX_POINTS], slope[MAX_POINTS];
    float span = p[n - 1].x - p[0].x;
    for (int k = 0; k < n; k++) {
        col[k] = roundf((p[k].x - p[0].x) / span * (float)(w - 1));
        if (k > 0 && col[k] <= col[k - 1]) {
            col[k] = col[k - 1] + 1; /* never two points on one column */
        }
        y[k] = p[k].y;
    }

    /* Fritsch-Carlson: secant slopes, averaged at each point but zero at a
     * peak or trough, then scaled down wherever they would overshoot. */
    for (int k = 0; k < n; k++) {
        float d_prev = k > 0 ? (y[k] - y[k - 1]) / (col[k] - col[k - 1]) : 0.0f;
        float d_next = k + 1 < n ? (y[k + 1] - y[k]) / (col[k + 1] - col[k]) : 0.0f;
        if (k == 0) {
            slope[k] = d_next;
        } else if (k == n - 1) {
            slope[k] = d_prev;
        } else {
            slope[k] = d_prev * d_next <= 0.0f ? 0.0f : (d_prev + d_next) / 2.0f;
        }
    }
    for (int k = 0; k + 1 < n; k++) {
        float d = (y[k + 1] - y[k]) / (col[k + 1] - col[k]);
        if (d == 0.0f) {
            slope[k] = slope[k + 1] = 0.0f;
            continue;
        }
        float a = slope[k] / d, b = slope[k + 1] / d;
        float r = a * a + b * b;
        if (r > 9.0f) {
            float t = 3.0f / sqrtf(r);
            slope[k] = t * a * d;
            slope[k + 1] = t * b * d;
        }
    }

    int k = 0;
    for (int c = 0; c < w; c++) {
        while (k + 2 < n && c > col[k + 1]) {
            k++;
        }
        float h = col[k + 1] - col[k];
        float t = fminf(fmaxf((c - col[k]) / h, 0.0f), 1.0f);
        float t2 = t * t, t3 = t2 * t;
        out[c] = (2 * t3 - 3 * t2 + 1) * y[k] + (t3 - 2 * t2 + t) * h * slope[k] +
                 (-2 * t3 + 3 * t2) * y[k + 1] + (t3 - t2) * h * slope[k + 1];
    }
}
