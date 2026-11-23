#include "node_fault.h"

#include <stddef.h>

enum node_action node_fault_action(enum node_liveness liveness,
                                   struct restart_limit *limit)
{
    if (liveness != NODE_DEAD) {
        return NODE_ACTION_WAIT;
    }
    if (limit == NULL || !restart_limit_on_death(limit)) {
        return NODE_ACTION_LOCK;
    }
    return NODE_ACTION_RESTART;
}
