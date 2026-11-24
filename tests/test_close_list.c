#include "close_list.h"

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

static struct issued_ring ring;
static struct signal_manifest_item out[SIGNAL_MANIFEST_CAP];

int main(void)
{
    unsigned i;
    unsigned n;
    uint64_t dropped[8];
    struct signal_manifest_item items[5];

    issued_ring_init(&ring);
    expect_u("empty ring", issued_ring_count(&ring), 0);
    for (i = 0; i < 6; i++) {
        expect_u("push", (unsigned)issued_ring_push(&ring, 100 + i,
                                                    SIGNAL_CLASS_URGENT,
                                                    (int64_t)i * 100),
                 0);
    }
    expect_u("six waiting", issued_ring_count(&ring), 6);
    n = issued_ring_closable(&ring, 350, 100, out, SIGNAL_CLOSE_CAP);
    expect_u("items 0..250 ns are old enough at 350 with settle 100", n, 3);
    expect_u("oldest first", out[0].number, 100);
    expect_u("in order", out[2].number, 102);
    expect_u("closable leaves the ring alone", issued_ring_count(&ring), 6);
    n = issued_ring_closable(&ring, 10000, 100, out, 2);
    expect_u("closable stops at max", n, 2);
    n = issued_ring_closable(&ring, 50, 100, out, SIGNAL_CLOSE_CAP);
    expect_u("nothing old enough", n, 0);
    issued_ring_drop_front(&ring, 3);
    expect_u("three left", issued_ring_count(&ring), 3);
    n = issued_ring_all(&ring, out, SIGNAL_MANIFEST_CAP);
    expect_u("all copies the rest", n, 3);
    expect_u("rest starts at 103", out[0].number, 103);
    issued_ring_drop_front(&ring, 99);
    expect_u("dropping more than held empties it", issued_ring_count(&ring), 0);

    issued_ring_init(&ring);
    for (i = 0; i < SIGNAL_MANIFEST_CAP; i++) {
        if (issued_ring_push(&ring, i + 1, SIGNAL_CLASS_BACKGROUND, 0) != 0) {
            break;
        }
    }
    expect_u("the ring holds the cap", issued_ring_count(&ring),
             SIGNAL_MANIFEST_CAP);
    expect_u("a full ring refuses", (unsigned)(issued_ring_push(&ring, 1, 1, 0) == -1), 1);
    issued_ring_drop_front(&ring, 10);
    for (i = 0; i < 10; i++) {
        issued_ring_push(&ring, SIGNAL_MANIFEST_CAP + 1 + i,
                         SIGNAL_CLASS_BACKGROUND, 1);
    }
    n = issued_ring_all(&ring, out, SIGNAL_MANIFEST_CAP);
    expect_u("wrapped ring keeps the order, first", out[0].number, 11);
    expect_u("wrapped ring keeps the order, last", out[n - 1].number,
             SIGNAL_MANIFEST_CAP + 10);

    for (i = 0; i < 5; i++) {
        items[i].number = 10 + i;
        items[i].class_id = SIGNAL_CLASS_BACKGROUND;
        items[i].reserved = 0;
    }
    dropped[0] = 12;
    dropped[1] = 99;
    dropped[2] = 14;
    dropped[3] = 3;
    expect_u("two of the drops are in the close",
             (unsigned)close_mark_dropped(items, 5, dropped, 4), 2);
    expect_u("12 is marked", items[2].reserved, SIGNAL_MANIFEST_DROPPED);
    expect_u("14 is marked", items[4].reserved, SIGNAL_MANIFEST_DROPPED);
    expect_u("13 is not", items[3].reserved, 0);
    items[1].number = 9;
    expect_u("unordered items are refused",
             (unsigned)(close_mark_dropped(items, 5, dropped, 4) == -1), 1);
    n = close_forget_dropped(dropped, 4, 10, 14);
    expect_u("forget keeps the two outside the close", n, 2);
    expect_u("99 stays", dropped[0], 99);
    expect_u("3 stays", dropped[1], 3);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
