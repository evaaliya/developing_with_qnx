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

#define SIGNAL_MANIFEST_CAP 64

/* Set on a manifest item the relay dropped. Other reserved values stay. */
#define SIGNAL_MANIFEST_DROPPED ((uint32_t)1)

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
