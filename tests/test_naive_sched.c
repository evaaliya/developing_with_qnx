/*
 * The priorities of both builds (D-084). Compiled twice by make test:
 * without SIGNAL_NAIVE the three classes keep their own priorities
 * (D-033); with -DSIGNAL_NAIVE=1 every class and the node's own
 * priority are the one naive priority, 10.
 */
#include "signal_sched.h"

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

int main(void)
{
#if SIGNAL_NAIVE
    expect_int("naive: build name", strcmp(SIGNAL_BUILD_NAME, "naive"), 0);
    expect_int("naive: own priority", SIGNAL_PRIO_OWN, 10);
    expect_int("naive: URGENT", signal_class_priority(SIGNAL_CLASS_URGENT), 10);
    expect_int("naive: COMMAND", signal_class_priority(SIGNAL_CLASS_COMMAND), 10);
    expect_int("naive: BACKGROUND", signal_class_priority(SIGNAL_CLASS_BACKGROUND), 10);
#else
    expect_int("ours: build name", strcmp(SIGNAL_BUILD_NAME, "ours"), 0);
    expect_int("ours: own priority", SIGNAL_PRIO_OWN, 10);
    expect_int("ours: URGENT", signal_class_priority(SIGNAL_CLASS_URGENT), 30);
    expect_int("ours: COMMAND", signal_class_priority(SIGNAL_CLASS_COMMAND), 20);
    expect_int("ours: BACKGROUND", signal_class_priority(SIGNAL_CLASS_BACKGROUND), 12);
#endif
    expect_int("unknown class", signal_class_priority(0), -1);
    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
