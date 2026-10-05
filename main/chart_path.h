/*
 * The line a chart draws through its candles' highs and lows. Pure, tested
 * on the host; ui.c draws the result.
 *
 * Points carry x in candles: candle i spans [i, i + 1).
 */
#pragma once


#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float x, y;
} chart_pt_t;

/**
 * @brief Each candle's low and high, then the last close: 2n + 1 points.
 *
 * In the order the price most likely visited them -- low then high for a
 * candle that closed up on the one before, high then low for one that closed
 * down -- at a quarter and three quarters through the candle. The exchange
 * does not say which came first; this is the usual way to draw candles as a
 * line.
 */
int chart_extremes(const float *close, const float *high, const float *low, int n,
                   chart_pt_t *out);

/**
 * @brief Only the swings of @p p: a high is kept if the price then falls by at
 * least @p min_frac of the whole range before rising past it, and a low
 * likewise. The wiggles between swings go; the highest high and lowest low
 * always stay, as do the first and last points.
 * @return points written to @p out (at most @p n)
 */
int chart_swings(const chart_pt_t *p, int n, float min_frac, chart_pt_t *out);

/**
 * @brief A smooth line through @p p as one value per column, @p w columns
 * from p[0].x to p[n-1].x.
 *
 * A monotone cubic (Fritsch-Carlson), which never overshoots a point: no peak
 * is invented and none is cut. Each point is first moved to its nearest
 * column, so its value is drawn exactly.
 */
void chart_columns(const chart_pt_t *p, int n, int w, float *out);

#ifdef __cplusplus
}
#endif
