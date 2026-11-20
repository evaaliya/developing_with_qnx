#include "restart_limit.h"

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

int main(void)
{
    struct restart_limit limit;

    restart_limit_init(&limit, 1);
    expect_int("K=1 is not locked before a death", limit.locked, 0);
    expect_int("K=1 does not restart the first death",
               restart_limit_on_death(&limit), 0);
    expect_int("K=1 is locked", limit.locked, 1);
    expect_int("a later death stays locked", restart_limit_on_death(&limit), 0);

    restart_limit_init(&limit, 3);
    expect_int("first of three restarts", restart_limit_on_death(&limit), 1);
    expect_int("streak is 1", limit.streak, 1);
    expect_int("second of three restarts", restart_limit_on_death(&limit), 1);
    expect_int("third death does not restart", restart_limit_on_death(&limit), 0);
    expect_int("three deaths lock", limit.locked, 1);
    restart_limit_on_progress(&limit);
    expect_int("progress does not clear a lock", limit.locked, 1);
    expect_int("streak stays at the limit", limit.streak, 3);

    restart_limit_init(&limit, 2);
    expect_int("death before progress restarts", restart_limit_on_death(&limit), 1);
    restart_limit_on_progress(&limit);
    expect_int("progress clears the streak", limit.streak, 0);
    expect_int("the next death restarts again", restart_limit_on_death(&limit), 1);
    expect_int("the death after that locks", restart_limit_on_death(&limit), 0);

    restart_limit_init(&limit, 0);
    expect_int("K below 1 starts locked", limit.locked, 1);
    expect_int("K below 1 does not restart", restart_limit_on_death(&limit), 0);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
