#include "close_list.h"

#include <stddef.h>
#include <string.h>

void issued_ring_init(struct issued_ring *ring)
{
    if (ring == NULL) {
        return;
    }
    memset(ring, 0, sizeof *ring);
}

static const struct issued_ring_item *at(const struct issued_ring *ring,
                                         unsigned i)
{
    return &ring->item[(ring->head + i) % SIGNAL_MANIFEST_CAP];
}

int issued_ring_push(struct issued_ring *ring, uint64_t number,
                     uint32_t class_id, int64_t issued_ns)
{
    struct issued_ring_item *slot;

    if (ring == NULL || ring->count >= SIGNAL_MANIFEST_CAP) {
        return -1;
    }
    slot = &ring->item[(ring->head + ring->count) % SIGNAL_MANIFEST_CAP];
    slot->number = number;
    slot->class_id = class_id;
    slot->reserved = 0;
    slot->issued_ns = issued_ns;
    /* The commit: the ring may live in shared memory and outlive a
       SIGKILL, so the item is whole before it is counted (D-063). */
    __atomic_store_n(&ring->count, ring->count + 1, __ATOMIC_RELEASE);
    return 0;
}

unsigned issued_ring_count(const struct issued_ring *ring)
{
    return ring == NULL ? 0 : ring->count;
}

static void copy_item(struct signal_manifest_item *out,
                      const struct issued_ring_item *in)
{
    out->number = in->number;
    out->class_id = in->class_id;
    out->reserved = 0;
}

unsigned issued_ring_closable(const struct issued_ring *ring, int64_t now_ns,
                              int64_t settle_ns,
                              struct signal_manifest_item *out, unsigned max)
{
    unsigned n = 0;

    if (ring == NULL || out == NULL || settle_ns < 0) {
        return 0;
    }
    while (n < max && n < ring->count) {
        const struct issued_ring_item *item = at(ring, n);

        if (item->issued_ns > now_ns - settle_ns) {
            break;
        }
        copy_item(&out[n], item);
        n++;
    }
    return n;
}

unsigned issued_ring_all(const struct issued_ring *ring,
                         struct signal_manifest_item *out, unsigned max)
{
    unsigned n = 0;

    if (ring == NULL || out == NULL) {
        return 0;
    }
    while (n < max && n < ring->count) {
        copy_item(&out[n], at(ring, n));
        n++;
    }
    return n;
}

void issued_ring_drop_front(struct issued_ring *ring, unsigned n)
{
    if (ring == NULL) {
        return;
    }
    if (n >= ring->count) {
        ring->head = (ring->head + ring->count) % SIGNAL_MANIFEST_CAP;
        ring->count = 0;
        return;
    }
    ring->head = (ring->head + n) % SIGNAL_MANIFEST_CAP;
    ring->count -= n;
}

int close_mark_dropped(struct signal_manifest_item *items, unsigned n,
                       const uint64_t *dropped, unsigned dropped_n)
{
    unsigned i;
    unsigned d;
    int marked = 0;

    if (items == NULL || (dropped == NULL && dropped_n > 0)) {
        return -1;
    }
    for (i = 1; i < n; i++) {
        if (items[i].number <= items[i - 1].number) {
            return -1;
        }
    }
    if (n == 0) {
        return 0;
    }
    for (d = 0; d < dropped_n; d++) {
        unsigned lo = 0;
        unsigned hi = n;
        uint64_t want = dropped[d];

        if (want < items[0].number || want > items[n - 1].number) {
            continue;
        }
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;

            if (items[mid].number < want) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < n && items[lo].number == want &&
            items[lo].reserved != SIGNAL_MANIFEST_DROPPED) {
            items[lo].reserved = SIGNAL_MANIFEST_DROPPED;
            marked++;
        }
    }
    return marked;
}

unsigned close_forget_dropped(uint64_t *dropped, unsigned dropped_n,
                              uint64_t lo, uint64_t hi)
{
    unsigned in;
    unsigned out = 0;

    if (dropped == NULL) {
        return 0;
    }
    for (in = 0; in < dropped_n; in++) {
        if (dropped[in] >= lo && dropped[in] <= hi) {
            continue;
        }
        dropped[out++] = dropped[in];
    }
    return out;
}
