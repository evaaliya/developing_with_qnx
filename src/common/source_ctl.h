#ifndef SOURCE_CTL_H
#define SOURCE_CTL_H

/*
 * Commands and issue timing for source. No QNX headers and no clock:
 * the caller passes the time and performs the send. D-053.
 */

#include "signal_msg.h"

#include <stddef.h>
#include <stdint.h>

#define SOURCE_DEV_PATH "/dev/signal-source"

/* Class periods for live mode. The counted burst does not use them. */
#define SOURCE_URGENT_PERIOD_NS 20000000LL
#define SOURCE_COMMAND_PERIOD_NS 800000000LL
#define SOURCE_BACKGROUND_PERIOD_NS 500000000LL

/* One timer wakes the live source this often. */
#define SOURCE_TICK_NS 10000000LL

/*
 * One batch, repeated for as long as flood stays on (D-059, D-060).
 * 72 is larger than the ordinary queue of 64. A batch every 50 ms is
 * 1440 BACKGROUND a second, faster than the relay chain forwards them,
 * so the queue does not drain between batches (D-060).
 * The counted burst does not use this.
 */
#define SOURCE_FLOOD_PERIOD_NS 50000000LL
#define SOURCE_FLOOD_BATCH 72u

/*
 * Issued numbers the flood may never take, so URGENT, COMMAND, and
 * ordinary BACKGROUND keep their place on the issued list after a long
 * flood. 2048 is about 38 s of the live schedule (53 signals a second).
 * When the flood reaches the rest of the list it turns itself off and
 * "flood on" is refused for the rest of the run (D-061).
 */
#define SOURCE_FLOOD_RESERVE 2048u

struct source_control {
    int live;
    int flood;
    int urgent_on;
    int stop;
    /* Set once the flood has used its share of the issued list (D-061). */
    int flood_spent;
    uint64_t issued_urgent;
    uint64_t issued_command;
    uint64_t issued_background;
    char error[96];
};

void source_ctl_init(struct source_control *ctl, int live);

/* 0 when the command is applied. -1 when it is rejected; flags other
 * than error are unchanged. A NULL ctl is rejected. */
int source_ctl_apply(struct source_control *ctl, const char *text);

void source_ctl_note_issued(struct source_control *ctl, uint32_t class_id);

/* Bytes written, excluding the NUL. -1 when the buffer is too small. */
int source_ctl_status(const struct source_control *ctl, char *buf, size_t cap);

/*
 * The class to send now, most urgent first. 0 when none is due.
 * Advances that class's next time to now + its period. A muted URGENT
 * is skipped and its next time stays, so restore does not wait out a
 * period that already passed. next_ns is indexed by class id - 1.
 */
uint32_t source_schedule_take(const struct source_control *ctl, int64_t now_ns,
                              int64_t next_ns[3]);

/* BACKGROUND signals to put in the next batch. 0 when flood is off,
 * the clock is missing, or the period has not elapsed. When a batch is
 * due, *next_ns becomes now_ns plus the period. There is no episode cap:
 * flood stays on until the caller turns it off (D-059). */
unsigned source_flood_due(int flood, int64_t now_ns, int64_t *next_ns);

/*
 * Flood signals that may still be issued, at most want. 0 once recorded
 * plus SOURCE_FLOOD_RESERVE reaches cap, or when cap is not larger than
 * the reserve (D-061).
 */
unsigned source_flood_room(unsigned long recorded, unsigned long cap,
                           unsigned want);

#endif
