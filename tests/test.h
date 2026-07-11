/* Minimal test harness: OK(cond) counts, failures print file:line. */
#ifndef MD_TEST_H
#define MD_TEST_H

#include <math.h>
#include <stdio.h>

static int t_pass, t_fail;

#define OK(cond)                                                     \
    do {                                                             \
        if (cond) {                                                  \
            t_pass++;                                                \
        } else {                                                     \
            t_fail++;                                                \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                            \
    } while (0)

#define APPROX(a, b) (fabs((double)(a) - (double)(b)) < 1e-9)

static inline int t_report(const char *suite)
{
    printf("%s: %d passed, %d failed\n", suite, t_pass, t_fail);
    return t_fail ? 1 : 0;
}

#endif
