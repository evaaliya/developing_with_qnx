#include "restart_limit.h"

#include <stddef.h>

void restart_limit_init(struct restart_limit *limit, int k)
{
    if (limit == NULL) {
        return;
    }
    limit->streak = 0;
    if (k < 1) {
        limit->limit = 0;
        limit->locked = 1;
        return;
    }
    limit->limit = k;
    limit->locked = 0;
}

int restart_limit_on_death(struct restart_limit *limit)
{
    if (limit == NULL || limit->locked) {
        return 0;
    }
    limit->streak++;
    if (limit->streak >= limit->limit) {
        limit->locked = 1;
        return 0;
    }
    return 1;
}

void restart_limit_on_progress(struct restart_limit *limit)
{
    if (limit == NULL || limit->locked) {
        return;
    }
    limit->streak = 0;
}
