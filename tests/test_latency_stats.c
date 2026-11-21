#include "latency_stats.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void expect_int(const char *name, int got, int want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %d, want %d)\n", name, got, want);
    failures++;
}

static void expect_i64(const char *name, int64_t got, int64_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %lld, want %lld)\n", name, (long long)got,
           (long long)want);
    failures++;
}

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %u, want %u)\n", name, got, want);
    failures++;
}

static void fill_range(struct latency_sample *sample, int64_t first,
                       uint32_t count)
{
    uint32_t i;

    latency_sample_init(sample);
    for (i = 0; i < count; i++) {
        if (latency_sample_add(sample, first + (int64_t)i) != 0) {
            printf("FAIL fill_range add %u\n", i);
            failures++;
            return;
        }
    }
}

int main(void)
{
    struct latency_sample sample;
    int64_t out;
    uint32_t i;
    int64_t kept_first;
    int64_t kept_last;

    latency_sample_init(&sample);
    out = 12345;
    expect_int("empty median fails", latency_stats_median(&sample, &out), -1);
    expect_i64("empty median leaves the output alone", out, 12345);
    out = 12345;
    expect_int("empty maximum fails", latency_stats_max(&sample, &out), -1);
    expect_i64("empty maximum leaves the output alone", out, 12345);
    out = 12345;
    expect_int("empty percentile fails", latency_stats_p99(&sample, &out), -1);
    expect_i64("empty percentile leaves the output alone", out, 12345);
    expect_int("null add fails", latency_sample_add(NULL, 1), -1);
    expect_int("null median fails", latency_stats_median(NULL, &out), -1);
    expect_int("null median output fails", latency_stats_median(&sample, NULL),
               -1);

    latency_sample_init(&sample);
    expect_int("add one value", latency_sample_add(&sample, 42), 0);
    expect_u32("one value is the whole sample", sample.count, 1);
    expect_int("median of one value", latency_stats_median(&sample, &out), 0);
    expect_i64("median of one value is that value", out, 42);
    expect_int("maximum of one value", latency_stats_max(&sample, &out), 0);
    expect_i64("maximum of one value is that value", out, 42);
    out = 12345;
    expect_int("percentile of one value is not available",
               latency_stats_p99(&sample, &out), 1);
    expect_i64("unavailable percentile leaves the output alone", out, 12345);

    /* Unsorted. Odd count: the middle rank after sorting. */
    latency_sample_init(&sample);
    expect_int("odd add 5", latency_sample_add(&sample, 5), 0);
    expect_int("odd add 1", latency_sample_add(&sample, 1), 0);
    expect_int("odd add 9", latency_sample_add(&sample, 9), 0);
    expect_int("odd add 3", latency_sample_add(&sample, 3), 0);
    expect_int("odd add 7", latency_sample_add(&sample, 7), 0);
    expect_int("odd median", latency_stats_median(&sample, &out), 0);
    expect_i64("odd median is the central value", out, 5);
    expect_int("odd maximum", latency_stats_max(&sample, &out), 0);
    expect_i64("odd maximum is the largest", out, 9);

    /* Even count: mean of the two central ranks, toward zero. */
    latency_sample_init(&sample);
    expect_int("even add 10", latency_sample_add(&sample, 10), 0);
    expect_int("even add 40", latency_sample_add(&sample, 40), 0);
    expect_int("even add 20", latency_sample_add(&sample, 20), 0);
    expect_int("even add 30", latency_sample_add(&sample, 30), 0);
    expect_int("even median", latency_stats_median(&sample, &out), 0);
    expect_i64("even median is the mean of the two central values", out, 25);

    latency_sample_init(&sample);
    expect_int("toward-zero add 2", latency_sample_add(&sample, 2), 0);
    expect_int("toward-zero add 3", latency_sample_add(&sample, 3), 0);
    expect_int("toward-zero median", latency_stats_median(&sample, &out), 0);
    expect_i64("even median rounds toward zero", out, 2);

    latency_sample_init(&sample);
    expect_int("negative add -5", latency_sample_add(&sample, -5), 0);
    expect_int("negative add -2", latency_sample_add(&sample, -2), 0);
    expect_int("negative median", latency_stats_median(&sample, &out), 0);
    expect_i64("even median of negatives rounds toward zero", out, -3);

    latency_sample_init(&sample);
    expect_int("wide add low", latency_sample_add(&sample, INT64_MAX - 1), 0);
    expect_int("wide add high", latency_sample_add(&sample, INT64_MAX), 0);
    expect_int("wide median", latency_stats_median(&sample, &out), 0);
    expect_i64("even median near the top of the range does not overflow", out,
               INT64_MAX - 1);

    /* 1..100. Rank ceil(0.99 * 100) = 99, so the value is 99, not 100. */
    fill_range(&sample, 1, 100);
    expect_u32("hundred values", sample.count, 100);
    expect_int("percentile of 1..100", latency_stats_p99(&sample, &out), 0);
    expect_i64("nearest rank of 1..100 is 99", out, 99);
    expect_int("median of 1..100", latency_stats_median(&sample, &out), 0);
    expect_i64("median of 1..100 is the mean of 50 and 51", out, 50);
    expect_int("maximum of 1..100", latency_stats_max(&sample, &out), 0);
    expect_i64("maximum of 1..100 is 100", out, 100);

    /* 1..105. Rank ceil(0.99 * 105) = 104. This is not n - 1. */
    fill_range(&sample, 1, 105);
    expect_int("percentile of 1..105", latency_stats_p99(&sample, &out), 0);
    expect_i64("nearest rank of 1..105 is 104", out, 104);

    fill_range(&sample, 1, 99);
    out = 12345;
    expect_int("percentile below 100 is not available",
               latency_stats_p99(&sample, &out), 1);
    expect_i64("percentile below 100 leaves the output alone", out, 12345);

    latency_sample_init(&sample);
    expect_int("maximum add -5", latency_sample_add(&sample, -5), 0);
    expect_int("maximum add -1", latency_sample_add(&sample, -1), 0);
    expect_int("maximum add -3", latency_sample_add(&sample, -3), 0);
    expect_int("maximum of negatives", latency_stats_max(&sample, &out), 0);
    expect_i64("maximum of negatives is the least negative", out, -1);

    fill_range(&sample, 0, LATENCY_STATS_CAP);
    expect_u32("sample filled to capacity", sample.count, LATENCY_STATS_CAP);
    kept_first = sample.value[0];
    kept_last = sample.value[LATENCY_STATS_CAP - 1];
    expect_int("one past capacity fails", latency_sample_add(&sample, -1), -1);
    expect_u32("overflow leaves the count alone", sample.count,
               LATENCY_STATS_CAP);
    expect_i64("overflow leaves the first value alone", sample.value[0],
               kept_first);
    expect_i64("overflow leaves the last value alone",
               sample.value[LATENCY_STATS_CAP - 1], kept_last);
    for (i = 0; i < LATENCY_STATS_CAP; i++) {
        if (sample.value[i] != (int64_t)i) {
            printf("FAIL overflow changed value %u\n", i);
            failures++;
            break;
        }
    }
    if (i == LATENCY_STATS_CAP) {
        printf("PASS overflow leaves every stored value alone\n");
    }

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
