#ifndef SIGNAL_MSG_H
#define SIGNAL_MSG_H

/*
 * Signal record and the deadline check. This header includes no QNX
 * headers and does not read a clock: the caller passes the time.
 * The file is not named signal.h, because -I src/common would then
 * hide the POSIX <signal.h>.
 */

#include <stdint.h>

enum signal_class {
    SIGNAL_CLASS_URGENT = 1,
    SIGNAL_CLASS_COMMAND = 2,
    SIGNAL_CLASS_BACKGROUND = 3
};

enum signal_verdict {
    SIGNAL_ON_TIME = 0,
    SIGNAL_LATE = 1
};

/* Stamps a relay adds. The sink turns neighbours into per-hop times. */
#define SIGNAL_MAX_HOPS 4

struct signal {
    uint64_t number;
    uint32_t class_id;
    uint32_t hop_count;
    int64_t issued_ns;
    int64_t deadline_ns;
    int64_t hop_ns[SIGNAL_MAX_HOPS];
};

/*
 * On time when now_ns <= issued_ns + deadline_ns, including the exact
 * boundary. A zero deadline is on time only at the issue instant.
 * A negative deadline is late. A NULL signal is late.
 */
enum signal_verdict signal_check(const struct signal *sig, int64_t now_ns);

const char *signal_class_name(uint32_t class_id);
const char *signal_verdict_name(enum signal_verdict verdict);

#endif
