#include "drop_notes.h"
#include "signal_msg.h"

#include <stdio.h>
#include <stdlib.h>

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

static struct drop_notes notes;
static struct drop_note out[DROP_NOTES_CAP + 8];

int main(void)
{
    unsigned i;
    unsigned n;
    unsigned long missed = 99;
    uint64_t first[3];

    drop_notes_init(&notes);
    expect_u("nothing pending at start", (unsigned)drop_notes_pending(&notes), 0);
    expect_u("take from empty", drop_notes_take(&notes, out, 8, &missed), 0);
    expect_u("no lines missed at start", missed, 0);

    /* A batch of 72 drops, kept in order with their own time. */
    for (i = 0; i < 72; i++) {
        expect_u("add a drop line",
                 (unsigned)(drop_notes_add(&notes, 1000 + i,
                                           SIGNAL_CLASS_BACKGROUND,
                                           (int64_t)5000 + i) + 1), 1);
    }
    expect_u("pending after adds", (unsigned)drop_notes_pending(&notes), 1);
    n = drop_notes_take(&notes, out, 50, &missed);
    expect_u("take is limited by max", n, 50);
    expect_u("oldest first", out[0].number, 1000);
    expect_u("time of the drop is kept", (unsigned long long)out[0].at_ns, 5000);
    expect_u("50th line", out[49].number, 1049);
    n = drop_notes_take(&notes, out, 50, &missed);
    expect_u("rest of the batch", n, 22);
    expect_u("last line", out[21].number, 1071);
    expect_u("nothing pending after taking all",
             (unsigned)drop_notes_pending(&notes), 0);

    /* A class that is not one of the three is refused. */
    expect_u("class 0 refused",
             (unsigned)(drop_notes_add(&notes, 1, 0, 0) == -1), 1);
    expect_u("class 4 refused for a notice",
             (unsigned)(drop_notes_want_announce(&notes, 4, 1) == -1), 1);

    /* Notices of one class that wait together become one. */
    expect_u("notice BACKGROUND",
             (unsigned)drop_notes_want_announce(&notes, SIGNAL_CLASS_BACKGROUND,
                                                200), 0);
    expect_u("notice BACKGROUND again",
             (unsigned)drop_notes_want_announce(&notes, SIGNAL_CLASS_BACKGROUND,
                                                272), 0);
    expect_u("notice COMMAND",
             (unsigned)drop_notes_want_announce(&notes, SIGNAL_CLASS_COMMAND,
                                                300), 0);
    expect_u("a notice alone is pending", (unsigned)drop_notes_pending(&notes), 1);
    expect_u("two classes to announce, one bit each",
             drop_notes_take_announce(&notes, first),
             (1u << (SIGNAL_CLASS_BACKGROUND - 1)) |
                 (1u << (SIGNAL_CLASS_COMMAND - 1)));
    expect_u("the first BACKGROUND number of the merged notices",
             first[SIGNAL_CLASS_BACKGROUND - 1], 200);
    expect_u("the COMMAND number", first[SIGNAL_CLASS_COMMAND - 1], 300);
    expect_u("no URGENT notice", first[SIGNAL_CLASS_URGENT - 1], 0);
    expect_u("announce cleared", drop_notes_take_announce(&notes, first), 0);

    /* A full ring keeps the oldest lines and counts the rest. */
    for (i = 0; i < DROP_NOTES_CAP; i++) {
        (void)drop_notes_add(&notes, i, SIGNAL_CLASS_BACKGROUND, (int64_t)i);
    }
    expect_u("ring full: next add refused",
             (unsigned)(drop_notes_add(&notes, 77777, SIGNAL_CLASS_BACKGROUND,
                                       0) == -1), 1);
    (void)drop_notes_add(&notes, 77778, SIGNAL_CLASS_BACKGROUND, 0);
    n = drop_notes_take(&notes, out, DROP_NOTES_CAP + 8, &missed);
    expect_u("full ring gives its capacity", n, DROP_NOTES_CAP);
    expect_u("missed lines reported", missed, 2);
    expect_u("first kept line", out[0].number, 0);
    expect_u("last kept line", out[DROP_NOTES_CAP - 1].number, DROP_NOTES_CAP - 1);
    n = drop_notes_take(&notes, out, 8, &missed);
    expect_u("missed count reset after a take", missed, 0);

    /* Wrap: head moves past the end of the array. */
    for (i = 0; i < 10; i++) {
        (void)drop_notes_add(&notes, 500 + i, SIGNAL_CLASS_BACKGROUND, 0);
    }
    (void)drop_notes_take(&notes, out, 6, &missed);
    for (i = 0; i < DROP_NOTES_CAP - 4; i++) {
        (void)drop_notes_add(&notes, 9000 + i, SIGNAL_CLASS_BACKGROUND, 0);
    }
    n = drop_notes_take(&notes, out, DROP_NOTES_CAP, &missed);
    expect_u("wrapped ring keeps everything that fit", n, DROP_NOTES_CAP);
    expect_u("wrapped ring order: first left from the earlier batch",
             out[0].number, 506);
    expect_u("wrapped ring order: last added", out[n - 1].number,
             9000 + DROP_NOTES_CAP - 5);
    expect_u("nothing missed while wrapping", missed, 0);

    expect_u("NULL notes is not pending",
             (unsigned)drop_notes_pending(NULL), 0);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
