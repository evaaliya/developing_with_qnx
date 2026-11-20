#ifndef RESTART_LIMIT_H
#define RESTART_LIMIT_H

/*
 * Consecutive deaths of one node. After K deaths with no progress
 * between them, the node is not restarted. No QNX headers.
 */

struct restart_limit {
    int limit;
    int streak;
    int locked;
};

void restart_limit_init(struct restart_limit *limit, int k);

/* 1: restart the node. 0: do not; state is locked. */
int restart_limit_on_death(struct restart_limit *limit);

/* A restarted node did useful work. Locked state does not clear. */
void restart_limit_on_progress(struct restart_limit *limit);

#endif
