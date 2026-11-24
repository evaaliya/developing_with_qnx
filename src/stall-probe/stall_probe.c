#define _QNX_SOURCE

/*
 * Guest stall probe. One thread on CPU 0 at priority 31, above every
 * signal class, wakes every 10 ms on an absolute CLOCK_MONOTONIC time
 * and prints each wake-up that came more than the threshold late. If
 * the network is late at the same moments, the whole guest stopped, not
 * the network (B22).
 *
 * usage: stall-probe <seconds> [threshold-ms]
 *        stall-probe now    prints CLOCK_MONOTONIC in ms and exits, so a
 *                           check can mark its own fault on the clock the
 *                           sink and the probe print (D-064)
 */

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/neutrino.h>

#define PERIOD_NS 10000000LL
#define PROBE_PRIORITY 31

static int64_t to_ns(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * 1000000000LL + (int64_t)ts->tv_nsec;
}

int main(int argc, char **argv)
{
    struct sched_param param;
    struct timespec next;
    struct timespec now;
    char *end = NULL;
    long seconds;
    long threshold_ms = 20;
    int64_t start_ns;
    int64_t until_ns;
    int64_t worst = 0;
    unsigned long wakes = 0;
    unsigned long stalls = 0;

    if (argc == 2 && strcmp(argv[1], "now") == 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            fprintf(stderr, "stall-probe: reading the clock failed: %s\n",
                    strerror(errno));
            return EXIT_FAILURE;
        }
        printf("%lld\n", (long long)(to_ns(&now) / 1000000LL));
        return EXIT_SUCCESS;
    }
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: stall-probe <seconds> [threshold-ms] | now\n");
        return EXIT_FAILURE;
    }
    seconds = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || seconds <= 0) {
        fprintf(stderr, "stall-probe: seconds must be a positive integer\n");
        return EXIT_FAILURE;
    }
    if (argc == 3) {
        threshold_ms = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || threshold_ms <= 0) {
            fprintf(stderr, "stall-probe: threshold must be a positive integer\n");
            return EXIT_FAILURE;
        }
    }
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "stall-probe: pinning to CPU 0 failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    memset(&param, 0, sizeof param);
    param.sched_priority = PROBE_PRIORITY;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        fprintf(stderr, "stall-probe: priority %d failed: %s\n",
                PROBE_PRIORITY, strerror(errno));
        return EXIT_FAILURE;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &next) != 0) {
        fprintf(stderr, "stall-probe: reading the clock failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    start_ns = to_ns(&next);
    until_ns = start_ns + (int64_t)seconds * 1000000000LL;
    printf("stall-probe: start %lld ms, period 10 ms, threshold %ld ms\n",
           (long long)(start_ns / 1000000LL), threshold_ms);
    fflush(stdout);
    for (;;) {
        int64_t due;
        int64_t late;

        due = to_ns(&next) + PERIOD_NS;
        next.tv_sec = (time_t)(due / 1000000000LL);
        next.tv_nsec = (long)(due % 1000000000LL);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) ==
               EINTR) {
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        late = to_ns(&now) - due;
        wakes++;
        if (late > worst) {
            worst = late;
        }
        if (late > (int64_t)threshold_ms * 1000000LL) {
            stalls++;
            printf("stall-probe: woke %lld ns late at %lld ms\n",
                   (long long)late, (long long)(due / 1000000LL));
            fflush(stdout);
            /* Start again from now, so one stall is one line. */
            next = now;
        }
        if (to_ns(&now) >= until_ns) {
            break;
        }
    }
    printf("stall-probe: wakes %lu, late over %ld ms %lu, worst %lld ns\n",
           wakes, threshold_ms, stalls, (long long)worst);
    return EXIT_SUCCESS;
}
