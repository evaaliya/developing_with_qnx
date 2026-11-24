#ifndef SIGNAL_WIRE_H
#define SIGNAL_WIRE_H

/*
 * Bytes sent along source, relay, and sink. No QNX headers: every
 * process compiles this with the same compiler, and each checks that
 * SIGNAL_MSG_TYPE is above _IO_MAX (0x1FF in sys/iomsg.h, described
 * as 0..511 on the QNX 8.0 name_attach page).
 * The signal body carries the hop stamps relays add (D-035).
 */

#include "signal_msg.h"

#include <stdint.h>

#define SIGNAL_ATTACH_NAME "signal-sink"
#define SIGNAL_MSG_TYPE ((uint16_t)512)
#define SIGNAL_MSG_MANIFEST ((uint16_t)513)
#define SIGNAL_MSG_LOCK ((uint16_t)514)
/* One receive admits many BACKGROUND signals before the forward thread runs. */
#define SIGNAL_MSG_BATCH ((uint16_t)515)
/*
 * A drop the relay already counted. Forwarded at once, outside the
 * queue, so the sink can enter DEGRADED while the flood is still on.
 * The ledger still closes the number from the manifest (D-059).
 */
#define SIGNAL_MSG_DROPPED ((uint16_t)516)
/*
 * Closes the oldest issued numbers while the network runs (D-062). The
 * same items as the manifest; relays mark their drops and pass it on
 * from their own close thread, outside the signal queue.
 */
#define SIGNAL_MSG_CLOSE ((uint16_t)517)

/*
 * Live S1 needs at least 100 URGENT (20 ms period, 5 s window) plus
 * COMMAND, ordinary BACKGROUND, and the flood batches (D-059). In live
 * mode this is the most numbers that may wait to be closed at once,
 * not a limit on the run (D-062).
 */
#define SIGNAL_MANIFEST_CAP 12288

/* Items in one close message. About 0.7 s of flood at 1440 a second. */
#define SIGNAL_CLOSE_CAP 1024

/* Larger than the ordinary relay queue, so one batch overflows it. */
#define SIGNAL_BATCH_MAX 72

/* Set on a manifest item the relay dropped. Other reserved values stay. */
#define SIGNAL_MANIFEST_DROPPED ((uint32_t)1)

/*
 * Set on the manifest header when a restarted source rebuilt the list
 * from lines the dead source had already printed (D-052).
 */
#define SIGNAL_MANIFEST_REBUILT ((uint32_t)1)

struct signal_manifest_item {
    uint64_t number;
    uint32_t class_id;
    uint32_t reserved;
};

struct signal_manifest {
    uint16_t type;
    uint16_t count;
    uint32_t reserved;
    struct signal_manifest_item item[SIGNAL_MANIFEST_CAP];
};

struct signal_close {
    uint16_t type;
    uint16_t count;
    uint32_t reserved;
    struct signal_manifest_item item[SIGNAL_CLOSE_CAP];
};

struct signal_batch {
    uint16_t type;
    uint16_t count;
    uint32_t reserved;
    struct signal body[SIGNAL_BATCH_MAX];
};

struct signal_dropped {
    uint16_t type;
    uint16_t reserved0;
    uint32_t class_id;
    uint64_t number;
};

struct signal_lock {
    uint16_t type;
    uint16_t reserved0;
    uint32_t reserved1;
};

struct signal_message {
    uint16_t type;
    uint16_t reserved0;
    uint32_t reserved1;
    struct signal body;
};

struct signal_reply {
    int32_t verdict;
    int32_t reserved;
    int64_t received_ns;
    int64_t transit_ns;
};

#endif
