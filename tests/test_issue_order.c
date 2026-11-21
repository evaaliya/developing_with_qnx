#include "issue_order.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_int(const char *name, int got, int want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %d, want %d)\n", name, got, want);
    failures++;
}

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %u, want %u)\n", name, got, want);
    failures++;
}

static int is_permutation(const uint32_t order[3])
{
    int background = 0;
    int command = 0;
    int urgent = 0;
    int i;

    for (i = 0; i < ISSUE_ORDER_TRIPLE; i++) {
        if (order[i] == SIGNAL_CLASS_BACKGROUND) {
            background++;
        } else if (order[i] == SIGNAL_CLASS_COMMAND) {
            command++;
        } else if (order[i] == SIGNAL_CLASS_URGENT) {
            urgent++;
        }
    }
    return background == 1 && command == 1 && urgent == 1;
}

static int count_class(const uint32_t *items, uint32_t count, uint32_t class_id)
{
    uint32_t i;
    int found = 0;

    for (i = 0; i < count; i++) {
        if (items[i] == class_id) {
            found++;
        }
    }
    return found;
}

static void expect_counts(const char *name, const uint32_t *items, uint32_t count,
                          uint32_t urgent, uint32_t command, uint32_t background)
{
    char label[96];

    snprintf(label, sizeof label, "%s urgent count", name);
    expect_int(label, count_class(items, count, SIGNAL_CLASS_URGENT),
               (int)urgent);
    snprintf(label, sizeof label, "%s command count", name);
    expect_int(label, count_class(items, count, SIGNAL_CLASS_COMMAND),
               (int)command);
    snprintf(label, sizeof label, "%s background count", name);
    expect_int(label, count_class(items, count, SIGNAL_CLASS_BACKGROUND),
               (int)background);
    snprintf(label, sizeof label, "%s length", name);
    expect_u32(label, count, urgent + command + background);
}

int main(void)
{
    uint32_t first[ISSUE_ORDER_TRIPLE];
    uint32_t second[ISSUE_ORDER_TRIPLE];
    uint32_t burst[64];
    uint32_t again[64];
    uint32_t written = 99;
    uint32_t seed;
    uint32_t index;
    int i;

    issue_order_triple(1, 0, NULL);
    printf("PASS null triple output is ignored\n");

    for (seed = 0; seed < 8; seed++) {
        for (index = 0; index < 4; index++) {
            char label[64];

            issue_order_triple(seed, index, first);
            issue_order_triple(seed, index, second);
            snprintf(label, sizeof label,
                     "seed %u index %u is one of each class", seed, index);
            expect_int(label, is_permutation(first), 1);
            snprintf(label, sizeof label, "seed %u index %u repeats", seed,
                     index);
            expect_int(label, memcmp(first, second, sizeof first) == 0, 1);
        }
    }

    issue_order_triple(1, 0, first);
    issue_order_triple(2, 0, second);
    expect_int("seed 1 and seed 2 differ at index 0",
               memcmp(first, second, sizeof first) != 0, 1);

    issue_order_triple(1, 0, first);
    issue_order_triple(1, 1, second);
    expect_int("seed 1 index 0 and index 1 differ",
               memcmp(first, second, sizeof first) != 0, 1);

    written = 99;
    burst[0] = 99;
    expect_int("null burst output fails",
               issue_order_burst(0, 0, 1, 1, 1, NULL, 3, &written), -1);
    expect_u32("null burst output leaves the count", written, 99);
    expect_int("null written fails",
               issue_order_burst(0, 0, 1, 1, 1, burst, 3, NULL), -1);
    expect_u32("null written leaves the buffer", burst[0], 99);

    written = 99;
    burst[0] = 77;
    expect_int("short capacity fails",
               issue_order_burst(0, 0, 1, 1, 1, burst, 2, &written), -1);
    expect_u32("short capacity leaves the count", written, 99);
    expect_u32("short capacity leaves the buffer", burst[0], 77);

    written = 99;
    expect_int("empty burst",
               issue_order_burst(0, 7, 0, 0, 0, burst, 4, &written), 0);
    expect_u32("empty burst writes nothing", written, 0);

    written = 0;
    expect_int("unseeded 2 urgent, 1 command, 3 background",
               issue_order_burst(0, 5, 2, 1, 3, burst, 64, &written), 0);
    expect_u32("unseeded length", written, 6);
    expect_u32("unseeded 0 background", burst[0], SIGNAL_CLASS_BACKGROUND);
    expect_u32("unseeded 1 command", burst[1], SIGNAL_CLASS_COMMAND);
    expect_u32("unseeded 2 urgent", burst[2], SIGNAL_CLASS_URGENT);
    expect_u32("unseeded 3 background", burst[3], SIGNAL_CLASS_BACKGROUND);
    expect_u32("unseeded 4 urgent", burst[4], SIGNAL_CLASS_URGENT);
    expect_u32("unseeded 5 background", burst[5], SIGNAL_CLASS_BACKGROUND);
    expect_counts("unseeded 2/1/3", burst, written, 2, 1, 3);

    expect_int("seed does not change the fixed order",
               issue_order_burst(0, 99, 2, 1, 3, again, 64, &written), 0);
    expect_int("fixed order ignores the seed",
               memcmp(burst, again, 6 * sizeof burst[0]) == 0, 1);

    written = 0;
    expect_int("seeded flood counts",
               issue_order_burst(1, 1, 3, 3, 40, burst, 64, &written), 0);
    expect_counts("seeded 3/3/40", burst, written, 3, 3, 40);
    expect_int("seeded triple 0", is_permutation(burst), 1);
    expect_int("seeded triple 1", is_permutation(burst + 3), 1);
    expect_int("seeded triple 2", is_permutation(burst + 6), 1);
    for (i = 9; i < 46; i++) {
        if (burst[i] != SIGNAL_CLASS_BACKGROUND) {
            printf("FAIL seeded tail stays background at %d\n", i);
            failures++;
            break;
        }
    }
    if (i == 46) {
        printf("PASS seeded tail is the remaining background\n");
    }

    written = 0;
    expect_int("same seed lays the flood out again",
               issue_order_burst(1, 1, 3, 3, 40, again, 64, &written), 0);
    expect_int("same seed repeats the flood",
               memcmp(burst, again, 46 * sizeof burst[0]) == 0, 1);

    written = 0;
    expect_int("other seed lays the flood out",
               issue_order_burst(1, 2, 3, 3, 40, again, 64, &written), 0);
    expect_int("other seed changes the flood",
               memcmp(burst, again, 46 * sizeof burst[0]) != 0, 1);
    expect_counts("other seed 3/3/40", again, written, 3, 3, 40);

    written = 0;
    expect_int("only urgent",
               issue_order_burst(1, 4, 1, 0, 0, burst, 64, &written), 0);
    expect_u32("only urgent length", written, 1);
    expect_u32("only urgent class", burst[0], SIGNAL_CLASS_URGENT);

    written = 0;
    expect_int("uneven seeded counts",
               issue_order_burst(1, 8, 1, 4, 2, burst, 64, &written), 0);
    expect_counts("seeded 1/4/2", burst, written, 1, 4, 2);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
