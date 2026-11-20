#ifndef NET_STATE_H
#define NET_STATE_H

/*
 * Network state from the product brief, section 6. No QNX headers
 * and no clock: the caller passes time. A late signal is never fresh.
 */

#include "signal_msg.h"

#include <stdint.h>

enum net_state {
    NET_INIT = 0,
    NET_NORMAL,
    NET_DEGRADED,
    NET_SAFE,
    NET_RECOVERING,
    NET_LOCKED_SAFE
};

enum net_presentation {
    NET_PRESENT_HELD = 0,
    NET_PRESENT_FRESH = 1,
    NET_PRESENT_LATE = 2
};

/* On-time URGENT signals in a row required to leave RECOVERING. */
#define NET_RECOVER_URGENT_NEED 3

struct net_machine {
    enum net_state state;
    int recover_need;
    int recover_streak;
    int64_t urgent_gap_ns;
    int64_t next_urgent_due_ns;
    int command_behind;
    int background_behind;
    int locked;
};

void net_machine_init(struct net_machine *machine, int64_t start_ns,
                      int64_t urgent_gap_ns, int recover_need);

/*
 * One arrived signal. presentation is never FRESH when the verdict
 * is late, and never FRESH while the path is still broken.
 * presentation may be NULL.
 */
enum net_state net_machine_on_signal(struct net_machine *machine,
                                     uint32_t class_id,
                                     enum signal_verdict verdict,
                                     int64_t now_ns,
                                     enum net_presentation *presentation);

/* No on-time URGENT arrived before its due time. */
enum net_state net_machine_on_tick(struct net_machine *machine, int64_t now_ns);

/* A BACKGROUND or COMMAND was dropped. URGENT is not dropped. */
enum net_state net_machine_on_dropped(struct net_machine *machine,
                                      uint32_t class_id);

/* Restart limit exhausted. Stays here. */
enum net_state net_machine_on_lock(struct net_machine *machine);

const char *net_state_name(enum net_state state);
const char *net_presentation_name(enum net_presentation presentation);

#endif
