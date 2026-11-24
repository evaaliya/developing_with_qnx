/*
 * Books that outlive a node (D-063): the live source book, the sink
 * ledger after a restart, the relay drop list, and the repeated close.
 * A death is a copy of the memory at the point the process stopped.
 */
#include "drop_book.h"
#include "live_book.h"
#include "signal_ledger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_u(const char *name, unsigned long long got,
                     unsigned long long want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %llu, want %llu)\n", name, got, want);
    failures++;
}

static struct live_book book;
static struct live_book after;
static struct signal_ledger ledger;
static struct signal_ledger expected;
static struct drop_book drops;
static struct signal_manifest_item items[SIGNAL_MANIFEST_CAP];

static uint32_t class_of(unsigned i)
{
    return (uint32_t)(i % 3u) + 1u;
}

static void live_book_tests(void)
{
    struct live_book_resume r;
    uint64_t number = 0;
    unsigned i;

    live_book_init(&book);
    expect_u("live book: first number is 1",
             (unsigned)live_book_take(&book, SIGNAL_CLASS_URGENT, 10, &number), 0);
    expect_u("live book: number 1", number, 1);
    for (i = 1; i < 30; i++) {
        (void)live_book_take(&book, class_of(i), 10 + (int64_t)i, &number);
    }
    expect_u("live book: 30 waiting", issued_ring_count(&book.ring), 30);
    expect_u("live book: URGENT issued 10",
             live_book_issued(&book, SIGNAL_CLASS_URGENT), 10);
    expect_u("live book: unknown class is refused",
             (unsigned)(live_book_take(&book, 7, 0, &number) != 0), 1);

    live_book_close_front(&book, 12);
    expect_u("live book: 18 left after a close of 12",
             issued_ring_count(&book.ring), 18);
    expect_u("live book: URGENT still issued 10 after the close",
             live_book_issued(&book, SIGNAL_CLASS_URGENT), 10);
    expect_u("live book: closed URGENT 4", book.closed[SIGNAL_CLASS_URGENT], 4);
    live_book_close_front(&book, 99);
    expect_u("live book: closing more than held empties it",
             issued_ring_count(&book.ring), 0);
    expect_u("live book: BACKGROUND issued 10 after all closed",
             live_book_issued(&book, SIGNAL_CLASS_BACKGROUND), 10);

    /* A restarted source goes on after the last number. */
    (void)live_book_take(&book, SIGNAL_CLASS_COMMAND, 50, &number);
    expect_u("live book: resume takes the book",
             (unsigned)live_book_resume(&book, &r), 0);
    expect_u("live book: resume next 32", r.next, 32);
    expect_u("live book: resume waiting 1", r.waiting, 1);
    expect_u("live book: resume COMMAND issued 11",
             r.issued[SIGNAL_CLASS_COMMAND], 11);
    expect_u("live book: resumes 1", book.resumes, 1);
    (void)live_book_take(&book, SIGNAL_CLASS_URGENT, 60, &number);
    expect_u("live book: no number repeats after resume", number, 32);

    /* Death after the ring commit and before next moved. */
    after = book;
    (void)issued_ring_push(&after.ring, after.next, SIGNAL_CLASS_URGENT, 70);
    expect_u("live book: resume after a death in take",
             (unsigned)live_book_resume(&after, &r), 0);
    expect_u("live book: next moved past the ring", r.next_moved, 1);
    expect_u("live book: next 34", r.next, 34);

    /* Death in the middle of a close: head written, count and closed not. */
    live_book_init(&book);
    for (i = 0; i < 20; i++) {
        (void)live_book_take(&book, class_of(i), (int64_t)i, &number);
    }
    after = book;
    live_book_close_front(&after, 8);
    book.journal = after.journal;
    book.journal.head = after.ring.head;
    book.journal.count = after.ring.count;
    memcpy(book.journal.closed, after.closed, sizeof book.journal.closed);
    book.journal.valid = 1;
    book.ring.head = after.ring.head;
    expect_u("live book: resume after a death in a close",
             (unsigned)live_book_resume(&book, &r), 0);
    expect_u("live book: the close was redone", r.journal_redone, 1);
    expect_u("live book: count as after the close", book.ring.count,
             after.ring.count);
    expect_u("live book: closed URGENT as after the close",
             book.closed[SIGNAL_CLASS_URGENT], after.closed[SIGNAL_CLASS_URGENT]);
    expect_u("live book: issued totals as after the close",
             r.issued[1] + r.issued[2] + r.issued[3], 20);

    /* Death before the journal was valid: the close never happened. */
    live_book_init(&book);
    for (i = 0; i < 5; i++) {
        (void)live_book_take(&book, SIGNAL_CLASS_BACKGROUND, 0, &number);
    }
    book.journal.head = 3;
    book.journal.count = 2;
    expect_u("live book: a half-written journal is ignored",
             (unsigned)live_book_resume(&book, &r), 0);
    expect_u("live book: all five still wait", r.waiting, 5);

    /* Not a book. */
    after = book;
    after.magic = 0;
    expect_u("live book: wrong magic is refused",
             (unsigned)(live_book_resume(&after, &r) != 0), 1);
    after = book;
    after.ring.item[(after.ring.head + 1) % SIGNAL_MANIFEST_CAP].number = 1;
    expect_u("live book: numbers out of order are refused",
             (unsigned)(live_book_resume(&after, &r) != 0), 1);
    after = book;
    after.ring.count = SIGNAL_MANIFEST_CAP + 1;
    expect_u("live book: a count above the ring is refused",
             (unsigned)(live_book_resume(&after, &r) != 0), 1);

    live_book_init(&book);
    for (i = 0; i < SIGNAL_MANIFEST_CAP; i++) {
        if (live_book_take(&book, SIGNAL_CLASS_BACKGROUND, 0, &number) != 0) {
            break;
        }
    }
    expect_u("live book: the ring holds CAP", i, SIGNAL_MANIFEST_CAP);
    expect_u("live book: a full ring refuses the next",
             (unsigned)(live_book_take(&book, SIGNAL_CLASS_URGENT, 0, &number) != 0),
             1);
    expect_u("live book: a refused take does not move next", book.next,
             SIGNAL_MANIFEST_CAP + 1);
}

static void counts(const struct signal_ledger *l, uint32_t c, unsigned out[4])
{
    signal_ledger_counts(l, c, &out[0], &out[1], &out[2], &out[3]);
}

static void expect_same_totals(const char *what, const struct signal_ledger *a,
                               const struct signal_ledger *b)
{
    char name[160];
    uint32_t c;

    for (c = SIGNAL_CLASS_URGENT; c <= SIGNAL_CLASS_BACKGROUND; c++) {
        unsigned x[4];
        unsigned y[4];
        int k;

        counts(a, c, x);
        counts(b, c, y);
        for (k = 0; k < 4; k++) {
            snprintf(name, sizeof name, "%s: class %u total %d", what, c, k);
            expect_u(name, x[k], y[k]);
        }
    }
    snprintf(name, sizeof name, "%s: unaccounted", what);
    expect_u(name, signal_ledger_unaccounted(a), signal_ledger_unaccounted(b));
    snprintf(name, sizeof name, "%s: arrived unissued", what);
    expect_u(name, signal_ledger_arrived_unissued(a),
             signal_ledger_arrived_unissued(b));
}

static void on_lost_count(uint64_t number, uint32_t class_id, void *arg)
{
    (void)number;
    (void)class_id;
    (*(unsigned *)arg)++;
}

/* A close of first..first+n-1; every third is marked dropped if BACKGROUND. */
static unsigned make_close(uint64_t first, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        items[i].number = first + i;
        items[i].class_id = class_of(i);
        items[i].reserved = 0;
    }
    return n;
}

static void ledger_tests(void)
{
    struct signal_ledger_resume r;
    struct signal_ledger_close_result res;
    unsigned lost_calls = 0;
    unsigned n;
    unsigned i;
    uint64_t k;

    signal_ledger_init(&ledger);
    /* 1..30 arrive except every fifth; 7 is marked dropped. */
    for (i = 1; i <= 30; i++) {
        if (i % 5 == 0) {
            continue;
        }
        (void)signal_ledger_receive(&ledger, i, class_of(i - 1),
                                    i % 7 == 0 ? SIGNAL_LATE : SIGNAL_ON_TIME);
    }
    n = make_close(1, 30);
    items[5].reserved = SIGNAL_MANIFEST_DROPPED; /* 6 arrived: refused */
    items[2].reserved = SIGNAL_MANIFEST_DROPPED; /* 3 arrived: refused */
    items[9].reserved = SIGNAL_MANIFEST_DROPPED; /* 10: URGENT, refused */
    items[4].reserved = SIGNAL_MANIFEST_DROPPED; /* 5: COMMAND, not arrived */
    expect_u("close: applied",
             (unsigned)signal_ledger_close(&ledger, items, n, on_lost_count,
                                           &lost_calls, &res), 0);
    expect_u("close: 5 is dropped", res.dropped, 1);
    expect_u("close: lost callbacks equal lost", lost_calls, res.lost);
    expect_u("close: marks on two arrivals and an URGENT are refused",
             res.drop_refused, 3);
    expect_u("close: nothing open", signal_ledger_unaccounted(&ledger), 0);
    expect_u("close: nothing repeated", res.repeated, 0);

    /* The same close again: a source that died before the answer. */
    expected = ledger;
    lost_calls = 0;
    expect_u("close again: applied",
             (unsigned)signal_ledger_close(&ledger, items, n, on_lost_count,
                                           &lost_calls, &res), 0);
    expect_u("close again: all 30 repeated", res.repeated, 30);
    expect_u("close again: no new loss", res.lost, 0);
    expect_same_totals("close again", &ledger, &expected);

    /* A close that half overlaps the last one. */
    n = make_close(21, 20);
    expect_u("overlap: applied",
             (unsigned)signal_ledger_close(&ledger, items, n, NULL, NULL, &res), 0);
    expect_u("overlap: 10 repeated", res.repeated, 10);
    expect_u("overlap: 10 new lost", res.lost, 10);

    /* Resume recounts every total from the slots. */
    expected = ledger;
    memset(ledger.count, 0x5a, sizeof ledger.count);
    ledger.open_count = 77;
    ledger.unissued_count = 9;
    expect_u("resume: takes the ledger",
             (unsigned)signal_ledger_resume(&ledger, &r), 0);
    expect_u("resume: resumes 1", ledger.resumes, 1);
    expect_same_totals("resume", &ledger, &expected);

    /* Whole-run totals through many hand-overs of slots. */
    signal_ledger_init(&ledger);
    for (k = 1; k <= 3 * SIGNAL_LEDGER_CAP; k += SIGNAL_CLOSE_CAP) {
        n = make_close(k, SIGNAL_CLOSE_CAP);
        for (i = 0; i < n; i += 2) {
            (void)signal_ledger_receive(&ledger, items[i].number,
                                        items[i].class_id, SIGNAL_ON_TIME);
        }
        if (signal_ledger_close(&ledger, items, n, NULL, NULL, &res) != 0) {
            break;
        }
    }
    expect_u("hand-over: three rings closed", k > 3 * SIGNAL_LEDGER_CAP, 1);
    expected = ledger;
    memset(ledger.count, 0, sizeof ledger.count);
    expect_u("hand-over: resume", (unsigned)signal_ledger_resume(&ledger, &r), 0);
    expect_same_totals("hand-over resume", &ledger, &expected);
    {
        unsigned c[4];

        counts(&ledger, SIGNAL_CLASS_URGENT, c);
        /* 36 closes of 1024; every third item, from the first, is URGENT. */
        expect_u("hand-over: URGENT on time + lost over the whole run",
                 c[0] + c[2], 36u * ((SIGNAL_CLOSE_CAP + 2u) / 3u));
    }

    /* Death in a hand-over after the journal: resume finishes it. */
    expected = ledger;
    k = 3 * SIGNAL_LEDGER_CAP + 1; /* its slot holds a settled older number */
    (void)signal_ledger_receive(&expected, k, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME);
    ledger.journal = expected.journal;
    memcpy(ledger.journal.retired, expected.retired, sizeof ledger.journal.retired);
    ledger.journal.retired_issued = expected.retired_issued;
    ledger.journal.index = (uint32_t)(k % SIGNAL_LEDGER_CAP);
    ledger.journal.number = k;
    ledger.journal.valid = 1;
    expect_u("cut hand-over: resume", (unsigned)signal_ledger_resume(&ledger, &r), 0);
    expect_u("cut hand-over: redone", r.journal_redone, 1);
    (void)signal_ledger_receive(&ledger, k, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME);
    expect_same_totals("cut hand-over", &ledger, &expected);

    /* Death in a receive between the class and the account. */
    expected = ledger;
    k = 3 * SIGNAL_LEDGER_CAP + 2;
    ledger.slot[k % SIGNAL_LEDGER_CAP].class_id = SIGNAL_CLASS_COMMAND;
    expect_u("cut receive: resume", (unsigned)signal_ledger_resume(&ledger, &r), 0);
    expect_same_totals("cut receive counts nothing", &ledger, &expected);

    expected = ledger;
    expected.magic = 1;
    expect_u("ledger: wrong magic is refused",
             (unsigned)(signal_ledger_resume(&expected, &r) != 0), 1);
    expected = ledger;
    expected.slot[5].number = 6;
    expect_u("ledger: a slot in the wrong place is refused",
             (unsigned)(signal_ledger_resume(&expected, &r) != 0), 1);
    expected = ledger;
    expected.slot[5].number = 5;
    expected.slot[5].account = SIGNAL_ACCOUNT_LOST;
    expected.slot[5].class_id = 9;
    expect_u("ledger: a bad class is refused",
             (unsigned)(signal_ledger_resume(&expected, &r) != 0), 1);

    signal_ledger_init(&ledger);
    (void)signal_ledger_issue(&ledger, 4, SIGNAL_CLASS_URGENT);
    expect_u("is issued: yes", (unsigned)signal_ledger_is_issued(&ledger, 4), 1);
    expect_u("is issued: no", (unsigned)signal_ledger_is_issued(&ledger, 5), 0);
}

static void drop_book_tests(void)
{
    char name[64];
    unsigned i;

    drop_book_init(&drops);
    for (i = 1; i <= 10; i++) {
        (void)drop_book_add(&drops, i * 10);
    }
    expect_u("drops: ten kept", drops.count, 10);
    drop_book_forget(&drops, 30, 70);
    expect_u("drops: 30..70 forgotten", drops.count, 5);
    expect_u("drops: order kept", drops.number[2], 80);
    expect_u("drops: resume", (unsigned)drop_book_resume(&drops), 0);
    expect_u("drops: resumes 1", drops.resumes, 1);
    drops.count = SIGNAL_MANIFEST_CAP + 1;
    expect_u("drops: a count above the list is refused",
             (unsigned)(drop_book_resume(&drops) != 0), 1);
    drop_book_init(&drops);
    for (i = 0; i < SIGNAL_MANIFEST_CAP; i++) {
        (void)drop_book_add(&drops, i + 1);
    }
    expect_u("drops: a full list refuses",
             (unsigned)(drop_book_add(&drops, 1) != 0), 1);
    expect_u("drops: store name",
             (unsigned)drop_book_store_name("signal-relay-a", name, sizeof name), 0);
    expect_u("drops: store name text", strcmp(name, "/signal-relay-a-drops") == 0, 1);
    expect_u("drops: a slash in the name is refused",
             (unsigned)(drop_book_store_name("a/b", name, sizeof name) != 0), 1);
    expect_u("drops: too long is refused",
             (unsigned)(drop_book_store_name("signal-relay-a", name, 8) != 0), 1);
}

int main(void)
{
    live_book_tests();
    ledger_tests();
    drop_book_tests();
    if (failures != 0) {
        printf("%d tests failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
