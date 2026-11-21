#ifndef LATENCY_STATS_H
#define LATENCY_STATS_H

/*
 * Fixed-capacity sample of latencies. No QNX headers, no clock, and no
 * malloc: the caller supplies every value (D-045). Statistics read the
 * sample and do not reorder it. A full sample rejects the new value and
 * stays as it was.
 *
 * An even median is the mean of the two central values, rounded toward
 * zero. The 99th percentile is the nearest rank, ceil(0.99 * n), counting
 * from 1. It is withheld when the sample has fewer than 100 values.
 */

#include <stdint.h>

#define LATENCY_STATS_CAP 4096
#define LATENCY_STATS_P99_MIN 100

struct latency_sample {
    uint32_t count;
    int64_t value[LATENCY_STATS_CAP];
};

void latency_sample_init(struct latency_sample *sample);

/* 0 on success. -1 if sample is NULL or the sample is already full. */
int latency_sample_add(struct latency_sample *sample, int64_t value);

/*
 * 0 and writes *out. -1 if sample or out is NULL, or the sample is empty,
 * or the stored count is past the capacity.
 */
int latency_stats_median(const struct latency_sample *sample, int64_t *out);
int latency_stats_max(const struct latency_sample *sample, int64_t *out);

/*
 * 0 and writes *out when count >= LATENCY_STATS_P99_MIN.
 * 1 when the sample is non-empty and smaller than that (not available).
 * -1 on the same invalid arguments as the median.
 */
int latency_stats_p99(const struct latency_sample *sample, int64_t *out);

#endif
