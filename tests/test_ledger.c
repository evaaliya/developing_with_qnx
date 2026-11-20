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

    signal_ledger_init(&ledger);
    expect_int("issue a background to drop",
               signal_ledger_issue(&ledger, 6, SIGNAL_CLASS_BACKGROUND), 0);
    expect_u("issued background is open", signal_ledger_unaccounted(&ledger), 1);
    expect_int("drop the issued background", signal_ledger_drop(&ledger, 6), 0);
    expect_int("the account is dropped",
               ledger.account[6] == SIGNAL_ACCOUNT_DROPPED, 1);
    expect_u("a drop is accounted", signal_ledger_unaccounted(&ledger), 0);
    expect_int("dropping it again fails", signal_ledger_drop(&ledger, 6), -1);
    expect_int("a second drop stays dropped",
               ledger.account[6] == SIGNAL_ACCOUNT_DROPPED, 1);

    signal_ledger_init(&ledger);
    expect_int("issue a background that arrives",
               signal_ledger_issue(&ledger, 7, SIGNAL_CLASS_BACKGROUND), 0);
    expect_int("receive before a drop",
               signal_ledger_receive(&ledger, 7, SIGNAL_CLASS_BACKGROUND,
                                     SIGNAL_ON_TIME),
               0);
    expect_int("an arrival cannot be dropped", signal_ledger_drop(&ledger, 7), -1);
    expect_int("arrival stays on time after a refused drop",
               ledger.account[7] == SIGNAL_ACCOUNT_ON_TIME, 1);
    expect_int("issue a late background",
               signal_ledger_issue(&ledger, 10, SIGNAL_CLASS_BACKGROUND), 0);
    expect_int("receive late before a drop",
               signal_ledger_receive(&ledger, 10, SIGNAL_CLASS_BACKGROUND,
                                     SIGNAL_LATE),
               0);
    expect_int("a late arrival cannot be dropped",
               signal_ledger_drop(&ledger, 10), -1);
    expect_int("late arrival stays late",
               ledger.account[10] == SIGNAL_ACCOUNT_LATE, 1);

    expect_int("an unissued number cannot be dropped",
               signal_ledger_drop(&ledger, 8), -1);
    expect_u("refusing an unissued drop adds nothing",
             signal_ledger_unaccounted(&ledger), 0);

    expect_int("issue an urgent",
               signal_ledger_issue(&ledger, 9, SIGNAL_CLASS_URGENT), 0);
    expect_int("an urgent cannot be dropped", signal_ledger_drop(&ledger, 9), -1);
    expect_int("urgent stays open", ledger.account[9] == SIGNAL_ACCOUNT_NONE, 1);
    expect_u("urgent is still unaccounted", signal_ledger_unaccounted(&ledger), 1);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
