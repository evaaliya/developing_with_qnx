#include "signal_queue.h"
#include "signal_sched.h"

#include <string.h>

static int class_rank(uint32_t class_id)
{
    switch (class_id) {
    case SIGNAL_CLASS_URGENT:
        return 0;
    case SIGNAL_CLASS_COMMAND:
        return 1;
    case SIGNAL_CLASS_BACKGROUND:
        return 2;
    default:
        return 3;
    }
}

/* Non-zero when a must leave before b. */
static int leaves_before(const struct signal_queue_item *a,
                         const struct signal_queue_item *b)
{
    int rank_a = class_rank(a->body.class_id);
    int rank_b = class_rank(b->body.class_id);

    if (rank_a != rank_b) {
        return rank_a < rank_b;
    }
    if (a->arrival_ns != b->arrival_ns) {
        return a->arrival_ns < b->arrival_ns;
    }
    return a->arrival_order < b->arrival_order;
}

/* Non-zero when a arrived after b. Same class is the caller's concern. */
static int arrived_later(const struct signal_queue_item *a,
                         const struct signal_queue_item *b)
{
    if (a->arrival_ns != b->arrival_ns) {
        return a->arrival_ns > b->arrival_ns;
    }
    return a->arrival_order > b->arrival_order;
}

static void remove_at(struct signal_queue *q, size_t index)
{
    size_t i;

    for (i = index + 1; i < q->count; i++) {
        q->items[i - 1] = q->items[i];
    }
    q->count--;
    memset(&q->items[q->count], 0, sizeof q->items[q->count]);
}

/*
 * Index of the item that leaves first among items of class only (0: any
 * class) whose class is not skip (0: skip none). -1 when there is none.
 */
static long best_index(const struct signal_queue *q, uint32_t only, uint32_t skip)
{
    long best = -1;
    size_t i;

    for (i = 0; i < q->count; i++) {
        uint32_t c = q->items[i].body.class_id;

        if ((only != 0 && c != only) || (skip != 0 && c == skip)) {
            continue;
        }
        if (best < 0 || leaves_before(&q->items[i], &q->items[best])) {
            best = (long)i;
        }
    }
    return best;
}

void signal_queue_init(struct signal_queue *q)
{
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof *q);
}

int signal_queue_push(struct signal_queue *q, const struct signal *sig,
                      int64_t arrival_ns, uint64_t arrival_order)
{
    struct signal_queue_item *item;

    if (q == NULL || sig == NULL || q->count >= SIGNAL_QUEUE_CAP) {
        return -1;
    }

    item = &q->items[q->count];
    item->body = *sig;
    item->arrival_ns = arrival_ns;
    item->arrival_order = arrival_order;
    q->count++;
    return 0;
}

int signal_queue_repick(struct signal_queue *q, struct signal_queue_item *held)
{
    return signal_queue_repick_except(q, 0, held);
}

int signal_queue_repick_except(struct signal_queue *q, uint32_t skip_class,
                               struct signal_queue_item *held)
{
    long best;
    int held_priority;
    int best_priority;
    struct signal_queue_item taken;

    if (q == NULL || held == NULL) {
        return -1;
    }
    best = best_index(q, 0, skip_class);
    if (best < 0) {
        return 0;
    }

    held_priority = signal_class_priority(held->body.class_id);
    best_priority = signal_class_priority(q->items[best].body.class_id);
    if (best_priority <= held_priority) {
        return 0;
    }

    taken = q->items[best];
    remove_at(q, (size_t)best);
    /*
     * remove_at freed one slot, so this push fits even when the queue
     * was at SIGNAL_QUEUE_CAP. The returned item keeps its arrival.
     */
    if (signal_queue_push(q, &held->body, held->arrival_ns,
                          held->arrival_order) != 0) {
        q->items[q->count] = taken;
        q->count++;
        return -1;
    }
    *held = taken;
    return 1;
}

static int pop_selected(struct signal_queue *q, uint32_t only, uint32_t skip,
                        struct signal_queue_item *out)
{
    long best;

    if (q == NULL || out == NULL) {
        return -1;
    }
    best = best_index(q, only, skip);
    if (best < 0) {
        return -1;
    }
    *out = q->items[best];
    remove_at(q, (size_t)best);
    return 0;
}

int signal_queue_pop(struct signal_queue *q, struct signal_queue_item *out)
{
    return pop_selected(q, 0, 0, out);
}

int signal_queue_pop_class(struct signal_queue *q, uint32_t class_id,
                           struct signal_queue_item *out)
{
    if (class_id == 0) {
        return -1;
    }
    return pop_selected(q, class_id, 0, out);
}

int signal_queue_pop_except(struct signal_queue *q, uint32_t skip_class,
                            struct signal_queue_item *out)
{
    return pop_selected(q, 0, skip_class, out);
}

int signal_queue_evict_newest(struct signal_queue *q, uint32_t class_id,
                              struct signal_queue_item *out)
{
    size_t best = 0;
    size_t i;
    int found = 0;

    if (class_id == SIGNAL_CLASS_URGENT) {
        return -1;
    }
    if (q == NULL || out == NULL || q->count == 0) {
        return -1;
    }

    for (i = 0; i < q->count; i++) {
        if (q->items[i].body.class_id != class_id) {
            continue;
        }
        if (!found || arrived_later(&q->items[i], &q->items[best])) {
            best = i;
            found = 1;
        }
    }
    if (!found) {
        return -1;
    }

    *out = q->items[best];
    remove_at(q, best);
    return 0;
}

int signal_queue_evict_background(struct signal_queue *q,
                                  struct signal_queue_item *out)
{
    return signal_queue_evict_newest(q, SIGNAL_CLASS_BACKGROUND, out);
}

int signal_queue_top_priority(const struct signal_queue *q)
{
    return signal_queue_top_priority_except(q, 0);
}

int signal_queue_top_priority_except(const struct signal_queue *q,
                                     uint32_t skip_class)
{
    long best;

    if (q == NULL) {
        return -1;
    }
    best = best_index(q, 0, skip_class);
    if (best < 0) {
        return -1;
    }
    return signal_class_priority(q->items[best].body.class_id);
}

size_t signal_queue_count_class(const struct signal_queue *q, uint32_t class_id)
{
    size_t n = 0;
    size_t i;

    if (q == NULL) {
        return 0;
    }
    for (i = 0; i < q->count; i++) {
        if (q->items[i].body.class_id == class_id) {
            n++;
        }
    }
    return n;
}

size_t signal_queue_count(const struct signal_queue *q)
{
    if (q == NULL) {
        return 0;
    }
    return q->count;
}

const struct signal_queue_item *signal_queue_item(const struct signal_queue *q,
                                                  size_t index)
{
    if (q == NULL || index >= q->count) {
        return NULL;
    }
    return &q->items[index];
}
