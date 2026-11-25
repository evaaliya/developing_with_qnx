#ifndef SIGNAL_SCHED_H
#define SIGNAL_SCHED_H

/*
 * Numeric priorities for the three classes. No QNX headers: the
 * processes pass these numbers to pthread_setschedparam. The usual
 * unprivileged ceiling is 63 (QNX_FACTS U10). Chosen in D-033.
 */

#include "signal_msg.h"

/*
 * The naive build (make naive, -DSIGNAL_NAIVE=1, D-084) is the same code
 * with one priority for every class and every node thread: 10, what a
 * QNX process gets without tuning. The relay then also takes its queue
 * in arrival order and drops the arriving signal of any class when full.
 */
#ifndef SIGNAL_NAIVE
#define SIGNAL_NAIVE 0
#endif

#if SIGNAL_NAIVE
#define SIGNAL_PRIO_NAIVE 10
#define SIGNAL_PRIO_OWN SIGNAL_PRIO_NAIVE
#define SIGNAL_PRIO_BACKGROUND SIGNAL_PRIO_NAIVE
#define SIGNAL_PRIO_COMMAND SIGNAL_PRIO_NAIVE
#define SIGNAL_PRIO_URGENT SIGNAL_PRIO_NAIVE
#define SIGNAL_BUILD_NAME "naive"
#else
#define SIGNAL_PRIO_OWN 10
#define SIGNAL_PRIO_BACKGROUND 12
#define SIGNAL_PRIO_COMMAND 20
#define SIGNAL_PRIO_URGENT 30
#define SIGNAL_BUILD_NAME "ours"
#endif

/* Negative when class_id is not one of the three classes. */
static inline int signal_class_priority(uint32_t class_id)
{
    switch (class_id) {
    case SIGNAL_CLASS_URGENT:
        return SIGNAL_PRIO_URGENT;
    case SIGNAL_CLASS_COMMAND:
        return SIGNAL_PRIO_COMMAND;
    case SIGNAL_CLASS_BACKGROUND:
        return SIGNAL_PRIO_BACKGROUND;
    default:
        return -1;
    }
}

#endif
