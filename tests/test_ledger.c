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
               signal_ledger_account(&ledger, 3) == SIGNAL_ACCOUNT_LOST, 1);
    expect_int("a second loss does not fail the books",
               signal_ledger_lose(&ledger, 3, SIGNAL_CLASS_BACKGROUND), 0);
    expect_int("an arrival is not rewritten as lost",
               signal_ledger_lose(&ledger, 1, SIGNAL_CLASS_URGENT), 0);
    expect_int("arrival stays on time",
               signal_ledger_account(&ledger, 1) == SIGNAL_ACCOUNT_ON_TIME, 1);

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
               signal_ledger_account(&ledger, 6) == SIGNAL_ACCOUNT_DROPPED, 1);
    expect_u("a drop is accounted", signal_ledger_unaccounted(&ledger), 0);
    expect_int("dropping it again fails", signal_ledger_drop(&ledger, 6), -1);
    expect_int("a second drop stays dropped",
               signal_ledger_account(&ledger, 6) == SIGNAL_ACCOUNT_DROPPED, 1);

    signal_ledger_init(&ledger);
    expect_int("issue a background that arrives",
               signal_ledger_issue(&ledger, 7, SIGNAL_CLASS_BACKGROUND), 0);
    expect_int("receive before a drop",
               signal_ledger_receive(&ledger, 7, SIGNAL_CLASS_BACKGROUND,
                                     SIGNAL_ON_TIME),
               0);
    expect_int("an arrival cannot be dropped", signal_ledger_drop(&ledger, 7), -1);
    expect_int("arrival stays on time after a refused drop",
               signal_ledger_account(&ledger, 7) == SIGNAL_ACCOUNT_ON_TIME, 1);
    expect_int("issue a late background",
               signal_ledger_issue(&ledger, 10, SIGNAL_CLASS_BACKGROUND), 0);
    expect_int("receive late before a drop",
               signal_ledger_receive(&ledger, 10, SIGNAL_CLASS_BACKGROUND,
                                     SIGNAL_LATE),
               0);
    expect_int("a late arrival cannot be dropped",
               signal_ledger_drop(&ledger, 10), -1);
    expect_int("late arrival stays late",
               signal_ledger_account(&ledger, 10) == SIGNAL_ACCOUNT_LATE, 1);

    expect_int("an unissued number cannot be dropped",
               signal_ledger_drop(&ledger, 8), -1);
    expect_u("refusing an unissued drop adds nothing",
             signal_ledger_unaccounted(&ledger), 0);

    expect_int("issue an urgent",
               signal_ledger_issue(&ledger, 9, SIGNAL_CLASS_URGENT), 0);
    expect_int("an urgent cannot be dropped", signal_ledger_drop(&ledger, 9), -1);
    expect_int("urgent stays open", signal_ledger_account(&ledger, 9) == SIGNAL_ACCOUNT_NONE, 1);
    expect_u("urgent is still unaccounted", signal_ledger_unaccounted(&ledger), 1);

    signal_ledger_init(&ledger);
    expect_int("issue a command to drop",
               signal_ledger_issue(&ledger, 11, SIGNAL_CLASS_COMMAND), 0);
    expect_u("issued command is open", signal_ledger_unaccounted(&ledger), 1);
    expect_int("drop the issued command", signal_ledger_drop(&ledger, 11), 0);
    expect_int("the command account is dropped",
               signal_ledger_account(&ledger, 11) == SIGNAL_ACCOUNT_DROPPED, 1);
    expect_u("a dropped command is accounted",
             signal_ledger_unaccounted(&ledger), 0);
    expect_int("dropping the command again fails",
               signal_ledger_drop(&ledger, 11), -1);

    signal_ledger_init(&ledger);
    expect_int("issue a command that arrives",
               signal_ledger_issue(&ledger, 12, SIGNAL_CLASS_COMMAND), 0);
    expect_int("receive the command before a drop",
               signal_ledger_receive(&ledger, 12, SIGNAL_CLASS_COMMAND,
                                     SIGNAL_ON_TIME),
               0);
    expect_int("an arrived command cannot be dropped",
               signal_ledger_drop(&ledger, 12), -1);
    expect_int("arrived command stays on time",
               signal_ledger_account(&ledger, 12) == SIGNAL_ACCOUNT_ON_TIME, 1);

    signal_ledger_init(&ledger);
    expect_int("arrival before any list",
               signal_ledger_receive(&ledger, 4, SIGNAL_CLASS_URGENT,
                                     SIGNAL_ON_TIME),
               0);
    expect_u("that arrival is not on a list",
             signal_ledger_arrived_unissued(&ledger), 1);
    expect_int("the rebuilt list names it",
               signal_ledger_issue(&ledger, 4, SIGNAL_CLASS_URGENT), 0);
    expect_int("the rebuilt list names one that never arrived",
               signal_ledger_issue(&ledger, 5, SIGNAL_CLASS_COMMAND), 0);
    expect_u("the named arrival is no longer unlisted",
             signal_ledger_arrived_unissued(&ledger), 0);
    expect_u("the missing named number is open",
             signal_ledger_unaccounted(&ledger), 1);
    expect_int("close the missing named number",
               signal_ledger_lose(&ledger, 5, SIGNAL_CLASS_COMMAND), 0);
    expect_u("rebuilt books close", signal_ledger_unaccounted(&ledger), 0);
    expect_int("an in-flight number the log did not print",
               signal_ledger_receive(&ledger, 6, SIGNAL_CLASS_URGENT,
                                     SIGNAL_LATE),
               0);
    expect_u("the unprinted arrival stays off the list",
             signal_ledger_arrived_unissued(&ledger), 1);

    {
        static struct signal_ledger ring;
        uint64_t far = 5 + SIGNAL_LEDGER_CAP;
        unsigned on_time;
        unsigned late;
        unsigned lost;
        unsigned dropped;

        signal_ledger_init(&ring);
        expect_int("a number past the cap is accepted",
                   signal_ledger_issue(&ring, 3 * SIGNAL_LEDGER_CAP + 1,
                                       SIGNAL_CLASS_URGENT), 0);
        expect_int("number 0 stays refused",
                   signal_ledger_issue(&ring, 0, SIGNAL_CLASS_URGENT), -1);

        signal_ledger_init(&ring);
        expect_int("arrival in slot 5",
                   signal_ledger_receive(&ring, 5, SIGNAL_CLASS_URGENT,
                                         SIGNAL_ON_TIME), 0);
        expect_int("an unsettled slot is not reused",
                   signal_ledger_receive(&ring, far, SIGNAL_CLASS_URGENT,
                                         SIGNAL_ON_TIME), -1);
        expect_u("the refusal is counted", ring.window_refused, 1);
        expect_int("close number 5", signal_ledger_issue(&ring, 5,
                                                         SIGNAL_CLASS_URGENT), 0);
        expect_int("a settled slot is reused by a newer number",
                   signal_ledger_receive(&ring, far, SIGNAL_CLASS_URGENT,
                                         SIGNAL_LATE), 0);
        expect_int("the old number has left the ring",
                   signal_ledger_account(&ring, 5) == SIGNAL_ACCOUNT_NONE, 1);
        expect_int("an older number cannot take a newer slot",
                   signal_ledger_issue(&ring, 5, SIGNAL_CLASS_URGENT), -1);
        signal_ledger_counts(&ring, SIGNAL_CLASS_URGENT, &on_time, &late, &lost,
                             &dropped);
        expect_u("totals keep the reused number on time", on_time, 1);
        expect_u("totals add the new late number", late, 1);
        expect_u("the newer arrival waits for its list",
                 signal_ledger_arrived_unissued(&ring), 1);

        signal_ledger_init(&ring);
        expect_int("issue a command that will be lost",
                   signal_ledger_issue(&ring, 40, SIGNAL_CLASS_COMMAND), 0);
        expect_int("is open", signal_ledger_is_open(&ring, 40), 1);
        expect_int("lose it", signal_ledger_lose(&ring, 40,
                                                 SIGNAL_CLASS_COMMAND), 0);
        expect_int("lost is not open", signal_ledger_is_open(&ring, 40), 0);
        signal_ledger_counts(&ring, SIGNAL_CLASS_COMMAND, &on_time, &late, &lost,
                             &dropped);
        expect_u("one lost command", lost, 1);
        expect_int("it arrives after the close",
                   signal_ledger_receive(&ring, 40, SIGNAL_CLASS_COMMAND,
                                         SIGNAL_LATE), 0);
        signal_ledger_counts(&ring, SIGNAL_CLASS_COMMAND, &on_time, &late, &lost,
                             &dropped);
        expect_u("the arrival replaces the loss", lost, 0);
        expect_u("and counts as late", late, 1);
        expect_u("still nothing open", signal_ledger_unaccounted(&ring), 0);
        expect_int("issue a background to drop",
                   signal_ledger_issue(&ring, 41, SIGNAL_CLASS_BACKGROUND), 0);
        expect_int("drop it", signal_ledger_drop(&ring, 41), 0);
        signal_ledger_counts(&ring, SIGNAL_CLASS_BACKGROUND, &on_time, &late,
                             &lost, &dropped);
        expect_u("one dropped background", dropped, 1);
        signal_ledger_counts(&ring, 9, &on_time, &late, &lost, &dropped);
        expect_u("an unknown class has no totals",
                 on_time + late + lost + dropped, 0);
    }

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
