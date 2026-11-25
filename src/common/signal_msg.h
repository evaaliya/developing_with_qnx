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

/*
 * What a signal says about the arm (D-080). URGENT carries the zone
 * sensor: SIGNAL_ARM_PERSON_IN while a person is in the zone, with the
 * time source took the "person in" event. COMMAND carries joint targets
 * in milliradians with SIGNAL_ARM_TARGET. A counted burst and BACKGROUND
 * carry no flags, and the sink ignores them for the arm.
 */
#define SIGNAL_ARM_PERSON_IN 1u
#define SIGNAL_ARM_TARGET 2u
/* Set on an URGENT issued at once for a zone change, not by the schedule. */
#define SIGNAL_ARM_EVENT 4u

struct signal_arm {
    uint32_t flags;
    int32_t joint_mrad[3];
    int64_t event_ns;
};

struct signal {
    uint64_t number;
    uint32_t class_id;
    uint32_t hop_count;
    int64_t issued_ns;
    int64_t deadline_ns;
    int64_t hop_ns[SIGNAL_MAX_HOPS];
    struct signal_arm arm;
};

/*
 * On time when now_ns <= issued_ns + deadline_ns, including the exact
 * boundary. A zero deadline is on time only at the issue instant.
 * A negative deadline is late. A NULL signal is late.
 */
/*
 * Class deadlines from calm live runs in B18 and B19 (D-058).
 * Twice the largest calm full latency of that class, rounded up to
 * 1 ms, then raised so URGENT <= COMMAND <= BACKGROUND and each
 * looser class is at least twice the previous one.
 */
#define SIGNAL_DEADLINE_URGENT_NS 31000000LL
#define SIGNAL_DEADLINE_COMMAND_NS 62000000LL
#define SIGNAL_DEADLINE_BACKGROUND_NS 124000000LL

enum signal_verdict signal_check(const struct signal *sig, int64_t now_ns);

/* The deadline for one of the three classes. -1 when class_id is not. */
int64_t signal_class_deadline_ns(uint32_t class_id);

const char *signal_class_name(uint32_t class_id);
const char *signal_verdict_name(enum signal_verdict verdict);

/*
 * 1 when SIGNAL_QUIET is "1": nodes skip the line per signal and keep
 * states, closes, losses, errors, and totals. A long live run otherwise
 * fills the guest's /tmp (62 MB) in about a minute of flood (D-062).
 */
int signal_log_quiet(void);

#endif
