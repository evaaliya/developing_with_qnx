#ifndef NODE_FAULT_H
#define NODE_FAULT_H

/*
 * What the supervisor does with one node. No QNX headers.
 * A stopped process is still alive: that is not a death.
 */

#include "restart_limit.h"

enum node_liveness {
    NODE_LIVE = 0,
    NODE_STOPPED = 1,
    NODE_DEAD = 2
};

enum node_action {
    NODE_ACTION_WAIT = 0,
    NODE_ACTION_RESTART = 1,
    NODE_ACTION_LOCK = 2
};

/*
 * LIVE and STOPPED wait, and do not change the limit.
 * DEAD asks the restart limit. NULL limit and DEAD locks.
 */
enum node_action node_fault_action(enum node_liveness liveness,
                                   struct restart_limit *limit);

#endif
