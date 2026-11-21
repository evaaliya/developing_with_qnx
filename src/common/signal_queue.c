#include "signal_queue.h"

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

int signal_queue_pop(struct signal_queue *q, struct signal_queue_item *out)
{
    size_t best;
    size_t i;

    if (q == NULL || out == NULL || q->count == 0) {
        return -1;
    }

    best = 0;
    for (i = 1; i < q->count; i++) {
        if (leaves_before(&q->items[i], &q->items[best])) {
            best = i;
        }
    }

    *out = q->items[best];
    remove_at(q, best);
    return 0;
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
