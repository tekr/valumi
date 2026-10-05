#include "chart_path.h"
#include "test.h"

static void test_extremes(void)
{
    /* Up on the candle before: low then high. Down: high then low. */
    const float close[] = {10, 12, 11};
    const float high[] = {11, 13, 12};
    const float low[] = {9, 10, 10};
    chart_pt_t p[7];
    CHECK_EQ_INT(chart_extremes(close, high, low, 3, p), 7);
    CHECK(p[2].y == 10 && p[3].y == 13); /* candle 1 rose: low, high */
    CHECK(p[4].y == 12 && p[5].y == 10); /* candle 2 fell: high, low */
    CHECK(p[2].x == 1.25f && p[3].x == 1.75f);
    CHECK(p[6].x == 3.0f && p[6].y == 11); /* ends at the last close */
}

static float lowest(const chart_pt_t *p, int n)
{
    float v = p[0].y;
    for (int i = 1; i < n; i++) {
        v = p[i].y < v ? p[i].y : v;
    }
    return v;
}

static float highest(const chart_pt_t *p, int n)
{
    float v = p[0].y;
    for (int i = 1; i < n; i++) {
        v = p[i].y > v ? p[i].y : v;
    }
    return v;
}

static void test_swings(void)
{
    /* A climb with a wiggle under 4% of the range, a drop to the bottom,
     * then a small bounce to finish. */
    chart_pt_t p[] = {{0, 50}, {1, 60}, {2, 59}, {3, 60.5f}, {4, 100}, {5, 99},
                      {6, 0},  {7, 1},  {8, 0.5f}, {9, 20}};
    chart_pt_t out[10];
    int n = chart_swings(p, 10, 0.04f, out);
    /* Start, the top, the bottom, the end: the 59/60.5 and 1/0.5 wiggles go. */
    CHECK_EQ_INT(n, 4);
    CHECK(out[0].y == 50 && out[1].y == 100 && out[2].y == 0 && out[3].y == 20);
    CHECK(highest(out, n) == highest(p, 10));
    CHECK(lowest(out, n) == lowest(p, 10));

    /* The extreme at the very end is kept. */
    chart_pt_t q[] = {{0, 10}, {1, 30}, {2, 20}, {3, 40}};
    n = chart_swings(q, 4, 0.04f, out);
    CHECK(out[n - 1].y == 40 && highest(out, n) == 40 && lowest(out, n) == 10);

    /* A low before the first big rise is kept even if barely below the
     * start. */
    chart_pt_t r[] = {{0, 50}, {1, 49}, {2, 100}, {3, 90}};
    n = chart_swings(r, 4, 0.04f, out);
    CHECK(lowest(out, n) == 49);

    /* Flat: just the ends. */
    chart_pt_t f[] = {{0, 5}, {1, 5}, {2, 5}};
    CHECK_EQ_INT(chart_swings(f, 3, 0.04f, out), 2);
}

static void test_columns(void)
{
    chart_pt_t p[] = {{0, 0}, {1, 10}, {2, 4}, {3, 8}};
    float col[31];
    /* Through every point exactly, never beyond the highest or the lowest,
     * and level at a peak. */
    chart_columns(p, 4, 31, col);
    CHECK(col[0] == 0 && col[10] == 10 && col[20] == 4 && col[30] == 8);
    for (int c = 0; c < 31; c++) {
        CHECK(col[c] >= 0 && col[c] <= 10);
    }
    CHECK(col[9] < 10 && col[11] < 10);

    /* A point between columns moves to the nearest one, keeping its value. */
    chart_pt_t q[] = {{0, 0}, {0.52f, 7}, {1, 1}};
    chart_columns(q, 3, 11, col);
    CHECK(col[5] == 7);
}

int main(void)
{
    RUN(test_extremes);
    RUN(test_swings);
    RUN(test_columns);
    TEST_MAIN_END("chart_path");
}
