#ifndef CLOSE_LIST_H
#define CLOSE_LIST_H

/*
 * Closing issued numbers while the live network runs (D-062).
 * No QNX headers and no clock: callers pass the time.
 *
 * source keeps every issued number in a ring until a close message for
 * it has come back through the chain. A number is closed once it was
 * issued at least CLOSE_SETTLE_NS ago: by then it has arrived, been
 * dropped by a relay, or is lost. A loss that arrives later still
 * replaces the loss in the sink ledger.
 */

#include "wire.h"

#include <stdint.h>

/* How often the live source sends a close. */
#define CLOSE_PERIOD_NS 250000000LL
/*
 * Age before a number is closed. The slowest BACKGROUND under the 50 ms
 * flood took 114 ms end to end (D-060); a relay that loses its next node
 * drains its queue as lost at once.
 */
#define CLOSE_SETTLE_NS 1000000000LL

struct issued_ring_item {
    uint64_t number;
    uint32_t class_id;
    uint32_t reserved;
    int64_t issued_ns;
};

struct issued_ring {
    struct issued_ring_item item[SIGNAL_MANIFEST_CAP];
    unsigned head;
    unsigned count;
};

void issued_ring_init(struct issued_ring *ring);

/* 0, or -1 when the ring is full or ring is NULL. */
int issued_ring_push(struct issued_ring *ring, uint64_t number,
                     uint32_t class_id, int64_t issued_ns);

unsigned issued_ring_count(const struct issued_ring *ring);

/*
 * Copies the oldest items issued at or before now_ns - settle_ns, in
 * order, at most max, into out (reserved 0). Stops at the first younger
 * item. Returns how many were copied; the ring is unchanged.
 */
unsigned issued_ring_closable(const struct issued_ring *ring, int64_t now_ns,
                              int64_t settle_ns,
                              struct signal_manifest_item *out, unsigned max);

/* Copies every item, oldest first, at most max. */
unsigned issued_ring_all(const struct issued_ring *ring,
                         struct signal_manifest_item *out, unsigned max);

/* Removes the n oldest items. n larger than the count empties the ring. */
void issued_ring_drop_front(struct issued_ring *ring, unsigned n);

/*
 * Marks the items whose number is in dropped with
 * SIGNAL_MANIFEST_DROPPED. Items must be in ascending number order, as
 * the ring makes them; otherwise nothing is marked and -1 is returned.
 * Returns the number of items marked.
 */
int close_mark_dropped(struct signal_manifest_item *items, unsigned n,
                       const uint64_t *dropped, unsigned dropped_n);

/* Removes from dropped every number in lo..hi. Returns the new count. */
unsigned close_forget_dropped(uint64_t *dropped, unsigned dropped_n,
                              uint64_t lo, uint64_t hi);

#endif
