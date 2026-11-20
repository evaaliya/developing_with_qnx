#include "signal_ledger.h"

#include <stdio.h>
#include <stdlib.h>

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

int main(void)
{
    struct signal_ledger ledger;

    signal_ledger_init(&ledger);
    expect_int("issue 1", signal_ledger_issue(&ledger, 1, SIGNAL_CLASS_URGENT), 0);
    expect_int("issue 2", signal_ledger_issue(&ledger, 2, SIGNAL_CLASS_COMMAND), 0);
    expect_int("issue 3",
               signal_ledger_issue(&ledger, 3, SIGNAL_CLASS_BACKGROUND), 0);
    expect_int("duplicate issue fails",
               signal_ledger_issue(&ledger, 1, SIGNAL_CLASS_URGENT), -1);
    expect_u("three issued are unaccounted", signal_ledger_unaccounted(&ledger), 3);

    expect_int("receive on time",
               signal_ledger_receive(&ledger, 1, SIGNAL_CLASS_URGENT,
                                     SIGNAL_ON_TIME),
               0);
    expect_int("receive late",
               signal_ledger_receive(&ledger, 2, SIGNAL_CLASS_COMMAND,
                                     SIGNAL_LATE),
               0);
    expect_int("lose the one that never arrived",
               signal_ledger_lose(&ledger, 3, SIGNAL_CLASS_BACKGROUND), 0);
    expect_u("nothing issued is silent", signal_ledger_unaccounted(&ledger), 0);
    expect_int("losing it again is still lost",
               ledger.account[3] == SIGNAL_ACCOUNT_LOST, 1);
    expect_int("a second loss does not fail the books",
               signal_ledger_lose(&ledger, 3, SIGNAL_CLASS_BACKGROUND), 0);
    expect_int("an arrival is not rewritten as lost",
               signal_ledger_lose(&ledger, 1, SIGNAL_CLASS_URGENT), 0);
    expect_int("arrival stays on time",
               ledger.account[1] == SIGNAL_ACCOUNT_ON_TIME, 1);

    signal_ledger_init(&ledger);
    expect_int("arrival before the manifest",
               signal_ledger_receive(&ledger, 4, SIGNAL_CLASS_URGENT,
                                     SIGNAL_ON_TIME),
               0);
    expect_int("manifest issues that arrival",
               signal_ledger_issue(&ledger, 4, SIGNAL_CLASS_URGENT), 0);
    expect_int("manifest issues a missing number",
               signal_ledger_issue(&ledger, 5, SIGNAL_CLASS_COMMAND), 0);
    expect_u("one number is still missing", signal_ledger_unaccounted(&ledger), 1);
    expect_int("close the missing number",
               signal_ledger_lose(&ledger, 5, SIGNAL_CLASS_COMMAND), 0);
    expect_u("books close", signal_ledger_unaccounted(&ledger), 0);
    expect_int("number 0 is rejected",
               signal_ledger_issue(&ledger, 0, SIGNAL_CLASS_URGENT), -1);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
