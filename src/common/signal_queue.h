#ifndef SIGNAL_QUEUE_H
#define SIGNAL_QUEUE_H

/*
 * Bounded queue of signals. No QNX headers and no clock: the caller
 * passes the arrival time and the arrival order. Not thread-safe.
 * Ordering and a full push that changes nothing are D-034.
 * Evicting the newest item of one class is D-042 and D-044.
 * URGENT is never evicted. Push itself never evicts.
 */

#include "signal_msg.h"

#include <stddef.h>
#include <stdint.h>

#define SIGNAL_QUEUE_CAP 64

struct signal_queue_item {
    struct signal body;
    int64_t arrival_ns;
    uint64_t arrival_order;
};

struct signal_queue {
    struct signal_queue_item items[SIGNAL_QUEUE_CAP];
    size_t count;
};

void signal_queue_init(struct signal_queue *q);

/* 0 on success. -1 if q or sig is NULL or the queue is already full. */
int signal_queue_push(struct signal_queue *q, const struct signal *sig,
                      int64_t arrival_ns, uint64_t arrival_order);

/*
 * Copies the most urgent item into out and removes it.
 * Same class: earlier arrival_ns, then smaller arrival_order.
 * 0 on success. -1 if q or out is NULL or the queue is empty.
 */
int signal_queue_pop(struct signal_queue *q, struct signal_queue_item *out);

/*
 * If q holds an item whose class is strictly more urgent than held
 * (signal_class_priority), that item replaces held. The previous held
 * returns to the queue with its own arrival_ns and arrival_order.
 * The item taken is the one signal_queue_pop would take. An equal
 * class does not swap, so the count stays the same and a full queue
 * still has room. 1 when held was replaced, 0 when nothing changed,
 * -1 when q or held is NULL.
 */
int signal_queue_repick(struct signal_queue *q, struct signal_queue_item *held);

/*
 * Removes the newest item of class_id: larger arrival_ns, then larger
 * arrival_order. SIGNAL_CLASS_URGENT always returns -1.
 * 0 on success. -1 if q or out is NULL, the queue is empty, class_id
 * is URGENT, or the queue holds none of that class.
 * On -1 the queue and out are unchanged.
 */
int signal_queue_evict_newest(struct signal_queue *q, uint32_t class_id,
                              struct signal_queue_item *out);

/* Newest BACKGROUND. Thin wrapper around signal_queue_evict_newest. */
int signal_queue_evict_background(struct signal_queue *q,
                                  struct signal_queue_item *out);

/*
 * Priority of the class that would leave first, via signal_class_priority.
 * -1 when q is NULL or the queue is empty.
 */
int signal_queue_top_priority(const struct signal_queue *q);

size_t signal_queue_count(const struct signal_queue *q);

/*
 * The relay has a forward thread for URGENT and one for the other
 * classes (D-074). These take part of the queue in the same order as
 * signal_queue_pop.
 *
 * signal_queue_pop_class: the item of class_id that leaves first.
 * signal_queue_pop_except, signal_queue_repick_except,
 * signal_queue_top_priority_except: as signal_queue_pop,
 * signal_queue_repick and signal_queue_top_priority, over the items
 * whose class is not skip_class.
 * Return values as the plain calls; an empty selection is -1 for pop
 * and top priority and 0 for repick.
 */
int signal_queue_pop_class(struct signal_queue *q, uint32_t class_id,
                           struct signal_queue_item *out);
int signal_queue_pop_except(struct signal_queue *q, uint32_t skip_class,
                            struct signal_queue_item *out);
int signal_queue_repick_except(struct signal_queue *q, uint32_t skip_class,
                               struct signal_queue_item *held);
int signal_queue_top_priority_except(const struct signal_queue *q,
                                     uint32_t skip_class);
size_t signal_queue_count_class(const struct signal_queue *q, uint32_t class_id);

/*
 * The naive build (D-084): the item with the smallest arrival_order,
 * whatever its class. 0 on success; -1 if q or out is NULL or the queue
 * is empty, and then out is unchanged.
 */
int signal_queue_pop_oldest(struct signal_queue *q, struct signal_queue_item *out);

/* NULL when q is NULL or index is past the end. Order is storage order. */
const struct signal_queue_item *signal_queue_item(const struct signal_queue *q,
                                                  size_t index);

#endif
