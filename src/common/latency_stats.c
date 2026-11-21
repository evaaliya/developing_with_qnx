#include "latency_stats.h"

#include <stddef.h>

static int usable(const struct latency_sample *sample, const int64_t *out)
{
    if (sample == NULL || out == NULL) {
        return 0;
    }
    if (sample->count == 0 || sample->count > LATENCY_STATS_CAP) {
        return 0;
    }
    return 1;
}

/* Insertion order is the arrival order. Sorting a copy keeps that. */
static void copy_sorted(const struct latency_sample *sample, int64_t *sorted)
{
    uint32_t i;

    for (i = 0; i < sample->count; i++) {
        int64_t incoming = sample->value[i];
        uint32_t slot = i;

        while (slot > 0 && sorted[slot - 1] > incoming) {
            sorted[slot] = sorted[slot - 1];
            slot--;
        }
        sorted[slot] = incoming;
    }
}

/*
 * (a + b) / 2 can overflow int64. Splitting each value keeps the sum of
 * two latencies inside the type. C division truncates toward zero.
 */
static int64_t mean_toward_zero(int64_t left, int64_t right)
{
    int64_t halves = (left / 2) + (right / 2);
    int64_t remainders = (left % 2) + (right % 2);

    return halves + (remainders / 2);
}

void latency_sample_init(struct latency_sample *sample)
{
    if (sample == NULL) {
        return;
    }
    sample->count = 0;
}

int latency_sample_add(struct latency_sample *sample, int64_t value)
{
    if (sample == NULL || sample->count >= LATENCY_STATS_CAP) {
        return -1;
    }
    sample->value[sample->count] = value;
    sample->count++;
    return 0;
}

int latency_stats_median(const struct latency_sample *sample, int64_t *out)
{
    int64_t sorted[LATENCY_STATS_CAP];
    uint32_t mid;

    if (!usable(sample, out)) {
        return -1;
    }
    copy_sorted(sample, sorted);
    mid = sample->count / 2;
    if ((sample->count % 2) == 1) {
        *out = sorted[mid];
        return 0;
    }
    *out = mean_toward_zero(sorted[mid - 1], sorted[mid]);
    return 0;
}

int latency_stats_max(const struct latency_sample *sample, int64_t *out)
{
    uint32_t i;
    int64_t max;

    if (!usable(sample, out)) {
        return -1;
    }
    max = sample->value[0];
    for (i = 1; i < sample->count; i++) {
        if (sample->value[i] > max) {
            max = sample->value[i];
        }
    }
    *out = max;
    return 0;
}

int latency_stats_p99(const struct latency_sample *sample, int64_t *out)
{
    int64_t sorted[LATENCY_STATS_CAP];
    uint32_t rank;

    if (!usable(sample, out)) {
        return -1;
    }
    if (sample->count < LATENCY_STATS_P99_MIN) {
        return 1;
    }
    copy_sorted(sample, sorted);
    /* ceil(99 * n / 100) = (99 * n + 99) / 100 for a positive n. */
    rank = (99u * sample->count + 99u) / 100u;
    if (rank == 0 || rank > sample->count) {
        return -1;
    }
    *out = sorted[rank - 1];
    return 0;
}
