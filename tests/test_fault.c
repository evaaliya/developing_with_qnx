#include "issued_log.h"
#include "node_fault.h"

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

static void expect_u(const char *name, unsigned got, unsigned want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %u, want %u)\n", name, got, want);
    failures++;
}

static void expect_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %llu, want %llu)\n", name,
           (unsigned long long)got, (unsigned long long)want);
    failures++;
}

int main(void)
{
    struct restart_limit limit;
    struct issued_item items[8];
    unsigned count = 99;
    uint64_t max_number = 99;
    uint64_t next = 99;
    unsigned total = 99;
    char line[128];
    const char *text =
        "source: SCHED_FIFO ceiling 10\n"
        "source: sent signal 1 class BACKGROUND\n"
        "source: sent signal 2 class COMMAND\n"
        "source: sent signal 3 class URGENT\n"
        "source: sent signal 4 class UR";
    const char *bad =
        "source: sent signal 1 class BACKGROUND\n"
        "source: sent signal 1 class COMMAND\n";
    const char *unknown =
        "source: sent signal 2 class NOPE\n";

    restart_limit_init(&limit, 3);
    expect_int("a live node waits",
               node_fault_action(NODE_LIVE, &limit) == NODE_ACTION_WAIT, 1);
    expect_int("live does not spend a death", limit.streak, 0);
    expect_int("a stopped node waits",
               node_fault_action(NODE_STOPPED, &limit) == NODE_ACTION_WAIT, 1);
    expect_int("stopped does not spend a death", limit.streak, 0);
    expect_int("a death restarts",
               node_fault_action(NODE_DEAD, &limit) == NODE_ACTION_RESTART, 1);
    expect_int("streak is 1", limit.streak, 1);
    expect_int("stopped after a death still waits",
               node_fault_action(NODE_STOPPED, &limit) == NODE_ACTION_WAIT, 1);
    expect_int("the stop did not add a death", limit.streak, 1);
    expect_int("second death restarts",
               node_fault_action(NODE_DEAD, &limit) == NODE_ACTION_RESTART, 1);
    expect_int("third death locks",
               node_fault_action(NODE_DEAD, &limit) == NODE_ACTION_LOCK, 1);
    expect_int("the limit is locked", limit.locked, 1);
    expect_int("a missing limit locks a death",
               node_fault_action(NODE_DEAD, NULL) == NODE_ACTION_LOCK, 1);
    expect_int("a missing limit still waits when stopped",
               node_fault_action(NODE_STOPPED, NULL) == NODE_ACTION_WAIT, 1);

    expect_int("parse keeps a trailing fragment",
               issued_list_parse(text, items, 8, &count, &max_number), 0);
    expect_u("three complete lines", count, 3);
    expect_u64("max is 3", max_number, 3);
    expect_u64("first number", items[0].number, 1);
    expect_int("first class", items[0].class_id == SIGNAL_CLASS_BACKGROUND, 1);
    expect_int("third class", items[2].class_id == SIGNAL_CLASS_URGENT, 1);
    expect_int("empty text",
               issued_list_parse("", items, 8, &count, &max_number), 0);
    expect_u("empty count", count, 0);
    expect_u64("empty max", max_number, 0);
    count = 7;
    max_number = 7;
    expect_int("a repeated number fails",
               issued_list_parse(bad, items, 8, &count, &max_number), -1);
    expect_u("failed parse leaves the count", count, 7);
    expect_u64("failed parse leaves the max", max_number, 7);
    expect_int("an unknown class fails",
               issued_list_parse(unknown, items, 8, &count, &max_number), -1);
    expect_int("null text fails",
               issued_list_parse(NULL, items, 8, &count, &max_number), -1);

    next = 5;
    expect_int("no printed line skips 1",
               issued_list_restart_number(0, 0, &next), 0);
    expect_u64("restart at 2", next, 2);
    expect_int("after 3 the next is 5",
               issued_list_restart_number(3, 1, &next), 0);
    expect_u64("skipped the in-flight number", next, 5);
    next = 9;
    expect_int("a max at the cap cannot restart",
               issued_list_restart_number(SIGNAL_LEDGER_CAP, 1, &next), -1);
    expect_u64("failed restart leaves next", next, 9);

    items[0].number = 1;
    items[0].class_id = SIGNAL_CLASS_BACKGROUND;
    items[1].number = 3;
    items[1].class_id = SIGNAL_CLASS_URGENT;
    expect_int("prior plus a new burst fit",
               issued_list_fits(items, 2, 5, 4, SIGNAL_MANIFEST_CAP, &total), 0);
    expect_u("total is prior plus new", total, 6);
    total = 3;
    expect_int("a new number must not reuse a prior one",
               issued_list_fits(items, 2, 3, 4, SIGNAL_MANIFEST_CAP, &total), -1);
    expect_u("a rejected fit leaves the total", total, 3);
    expect_int("the line round-trips",
               issued_list_line(&items[1], line, sizeof line), 0);
    expect_int("the line is the log line",
               strcmp(line, "source: sent signal 3 class URGENT\n") == 0, 1);
    expect_int("rebuilt flag",
               issued_list_is_rebuilt(SIGNAL_MANIFEST_REBUILT), 1);
    expect_int("a plain manifest is not rebuilt",
               issued_list_is_rebuilt(0), 0);
    expect_int("the sentence names the missing list",
               strstr(issued_list_rebuilt_sentence(),
                      "died before its issued list arrived") != NULL,
               1);
    expect_int("the sentence names what it did not see",
               strstr(issued_list_rebuilt_sentence(),
                      "no printed line") != NULL,
               1);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
