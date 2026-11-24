#include "live_book.h"

#include <stddef.h>
#include <string.h>

static int class_ok(uint32_t class_id)
{
    return class_id >= SIGNAL_CLASS_URGENT && class_id <= SIGNAL_CLASS_BACKGROUND;
}

static const struct issued_ring_item *ring_at(const struct issued_ring *ring,
                                              unsigned i)
{
    return &ring->item[(ring->head + i) % SIGNAL_MANIFEST_CAP];
}

void live_book_init(struct live_book *book)
{
    if (book == NULL) {
        return;
    }
    memset(book, 0, sizeof *book);
    issued_ring_init(&book->ring);
    book->magic = LIVE_BOOK_MAGIC;
    book->version = LIVE_BOOK_VERSION;
    book->size = (uint32_t)sizeof *book;
    book->next = 1;
    book->mode = LIVE_BOOK_MODE_SET;
}

void live_book_set_mode(struct live_book *book, int flood, int urgent_on)
{
    uint32_t mode = LIVE_BOOK_MODE_SET;

    if (book == NULL) {
        return;
    }
    if (flood) {
        mode |= LIVE_BOOK_MODE_FLOOD;
    }
    if (!urgent_on) {
        mode |= LIVE_BOOK_MODE_MUTE;
    }
    __atomic_store_n(&book->mode, mode, __ATOMIC_RELEASE);
}

int live_book_take(struct live_book *book, uint32_t class_id, int64_t now_ns,
                   uint64_t *number)
{
    uint64_t n;

    if (book == NULL || number == NULL || !class_ok(class_id)) {
        return -1;
    }
    n = book->next;
    if (issued_ring_push(&book->ring, n, class_id, now_ns) != 0) {
        return -1;
    }
    /* After the ring commit: a death here leaves next behind the ring,
       and resume moves it on. */
    __atomic_store_n(&book->next, n + 1, __ATOMIC_RELEASE);
    *number = n;
    return 0;
}

void live_book_close_front(struct live_book *book, unsigned n)
{
    struct live_book_journal *j;
    unsigned i;

    if (book == NULL) {
        return;
    }
    if (n > book->ring.count) {
        n = book->ring.count;
    }
    if (n == 0) {
        return;
    }
    j = &book->journal;
    for (i = 0; i < 4; i++) {
        j->closed[i] = book->closed[i];
    }
    for (i = 0; i < n; i++) {
        uint32_t c = ring_at(&book->ring, i)->class_id;

        if (class_ok(c)) {
            j->closed[c]++;
        }
    }
    j->head = (book->ring.head + n) % SIGNAL_MANIFEST_CAP;
    j->count = book->ring.count - n;
    j->reserved = 0;
    __atomic_store_n(&j->valid, 1u, __ATOMIC_RELEASE);
    book->ring.head = j->head;
    book->ring.count = j->count;
    for (i = 0; i < 4; i++) {
        book->closed[i] = j->closed[i];
    }
    __atomic_store_n(&j->valid, 0u, __ATOMIC_RELEASE);
}

uint64_t live_book_issued(const struct live_book *book, uint32_t class_id)
{
    uint64_t total;
    unsigned i;

    if (book == NULL || !class_ok(class_id)) {
        return 0;
    }
    total = book->closed[class_id];
    for (i = 0; i < book->ring.count && i < SIGNAL_MANIFEST_CAP; i++) {
        if (ring_at(&book->ring, i)->class_id == class_id) {
            total++;
        }
    }
    return total;
}

int live_book_resume(struct live_book *book, struct live_book_resume *out)
{
    unsigned i;
    uint64_t last = 0;

    if (book == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (book->magic != LIVE_BOOK_MAGIC || book->version != LIVE_BOOK_VERSION ||
        book->size != (uint32_t)sizeof *book) {
        return -1;
    }
    if (book->journal.valid) {
        if (book->journal.head >= SIGNAL_MANIFEST_CAP ||
            book->journal.count > SIGNAL_MANIFEST_CAP) {
            return -1;
        }
        book->ring.head = book->journal.head;
        book->ring.count = book->journal.count;
        for (i = 0; i < 4; i++) {
            book->closed[i] = book->journal.closed[i];
        }
        book->journal.valid = 0;
        out->journal_redone = 1;
    }
    if (book->ring.head >= SIGNAL_MANIFEST_CAP ||
        book->ring.count > SIGNAL_MANIFEST_CAP || book->next == 0) {
        return -1;
    }
    for (i = 0; i < book->ring.count; i++) {
        const struct issued_ring_item *item = ring_at(&book->ring, i);

        if (item->number == 0 || item->number <= last ||
            !class_ok(item->class_id)) {
            return -1;
        }
        last = item->number;
    }
    if (last >= book->next) {
        book->next = last + 1;
        out->next_moved = 1;
    }
    {
        uint32_t mode = __atomic_load_n(&book->mode, __ATOMIC_ACQUIRE);
        uint32_t known = LIVE_BOOK_MODE_SET | LIVE_BOOK_MODE_FLOOD |
                         LIVE_BOOK_MODE_MUTE;

        /* A word with other bits is not a mode a source wrote. */
        if ((mode & LIVE_BOOK_MODE_SET) != 0 && (mode & ~known) == 0) {
            out->mode_known = 1;
            out->flood = (mode & LIVE_BOOK_MODE_FLOOD) != 0;
            out->urgent_on = (mode & LIVE_BOOK_MODE_MUTE) == 0;
        } else {
            out->urgent_on = 1;
        }
    }
    book->resumes++;
    out->next = book->next;
    out->waiting = book->ring.count;
    for (i = SIGNAL_CLASS_URGENT; i <= SIGNAL_CLASS_BACKGROUND; i++) {
        out->issued[i] = live_book_issued(book, i);
    }
    return 0;
}
