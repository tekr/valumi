/* A deliberately tiny test harness: a failed CHECK prints where and why and
 * counts; TEST_MAIN exits non-zero if anything failed. */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fails;
static const char *g_test = "";

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        g_checks++;                                                                        \
        if (!(cond)) {                                                                     \
            g_fails++;                                                                     \
            fprintf(stderr, "FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_test, #cond);    \
        }                                                                                  \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                 \
    do {                                                                                   \
        long long _a = (long long)(a), _b = (long long)(b);                                \
        g_checks++;                                                                        \
        if (_a != _b) {                                                                    \
            g_fails++;                                                                     \
            fprintf(stderr, "FAIL %s:%d [%s] %s == %s  (%lld != %lld)\n", __FILE__,        \
                    __LINE__, g_test, #a, #b, _a, _b);                                     \
        }                                                                                  \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                                 \
    do {                                                                                   \
        const char *_a = (a), *_b = (b);                                                   \
        g_checks++;                                                                        \
        if (strcmp(_a, _b) != 0) {                                                         \
            g_fails++;                                                                     \
            fprintf(stderr, "FAIL %s:%d [%s] %s == %s  (\"%s\" != \"%s\")\n", __FILE__,    \
                    __LINE__, g_test, #a, #b, _a, _b);                                     \
        }                                                                                  \
    } while (0)

#define RUN(fn)                                                                            \
    do {                                                                                   \
        g_test = #fn;                                                                      \
        fn();                                                                              \
    } while (0)

#define TEST_MAIN_END(name)                                                                \
    printf("%-20s %s  (%d checks, %d failed)\n", name, g_fails ? "FAILED" : "ok", g_checks, \
           g_fails);                                                                       \
    return g_fails ? 1 : 0
