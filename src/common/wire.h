#ifndef SIGNAL_WIRE_H
#define SIGNAL_WIRE_H

/*
 * Bytes sent from source to sink. No QNX headers: both processes
 * compile this with the same compiler, and each checks that
 * SIGNAL_MSG_TYPE is above _IO_MAX (0x1FF in sys/iomsg.h, described
 * as 0..511 on the QNX 8.0 name_attach page).
 */

#include "signal_msg.h"

#include <stdint.h>

#define SIGNAL_ATTACH_NAME "signal-sink"
#define SIGNAL_MSG_TYPE ((uint16_t)512)

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
