#include "net_state.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void expect_state(const char *name, enum net_state got, enum net_state want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %s, want %s)\n", name, net_state_name(got),
           net_state_name(want));
    failures++;
}

static void expect_present(const char *name, enum net_presentation got,
                           enum net_presentation want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %s, want %s)\n", name, net_presentation_name(got),
           net_presentation_name(want));
    failures++;
}

static struct net_machine machine_at(int64_t start_ns, int recover_need)
{
    struct net_machine machine;

    net_machine_init(&machine, start_ns, 1000, recover_need);
    return machine;
}

int main(void)
{
    struct net_machine machine;
    enum net_presentation how = NET_PRESENT_FRESH;

    machine = machine_at(0, 3);
    expect_state("starts in INIT", machine.state, NET_INIT);
    expect_state("tick on the due instant stays INIT",
                 net_machine_on_tick(&machine, 1000), NET_INIT);
    expect_state("tick after the due instant is SAFE",
                 net_machine_on_tick(&machine, 1001), NET_SAFE);

    machine = machine_at(0, 3);
    how = NET_PRESENT_LATE;
    expect_state("first on-time URGENT leaves INIT",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_ON_TIME, 100, &how),
                 NET_NORMAL);
    expect_present("first on-time URGENT is fresh", how, NET_PRESENT_FRESH);

    machine = machine_at(0, 3);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 100,
                          NULL);
    how = NET_PRESENT_FRESH;
    expect_state("late COMMAND is DEGRADED",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_COMMAND,
                                       SIGNAL_LATE, 200, &how),
                 NET_DEGRADED);
    expect_present("late COMMAND is not fresh", how, NET_PRESENT_LATE);
    how = NET_PRESENT_LATE;
    expect_state("on-time URGENT keeps DEGRADED",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_ON_TIME, 300, &how),
                 NET_DEGRADED);
    expect_present("on-time URGENT in DEGRADED is fresh", how, NET_PRESENT_FRESH);
    expect_state("on-time COMMAND clears DEGRADED",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_COMMAND,
                                       SIGNAL_ON_TIME, 400, &how),
                 NET_NORMAL);

    machine = machine_at(0, 3);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 100,
                          NULL);
    net_machine_on_signal(&machine, SIGNAL_CLASS_COMMAND, SIGNAL_LATE, 200,
                          NULL);
    net_machine_on_signal(&machine, SIGNAL_CLASS_BACKGROUND, SIGNAL_LATE, 250,
                          NULL);
    expect_state("both classes behind stays DEGRADED", machine.state, NET_DEGRADED);
    expect_state("one cleared class stays DEGRADED",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_COMMAND,
                                       SIGNAL_ON_TIME, 300, NULL),
                 NET_DEGRADED);
    expect_state("both cleared classes return to NORMAL",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_BACKGROUND,
                                       SIGNAL_ON_TIME, 400, NULL),
                 NET_NORMAL);

    machine = machine_at(0, 3);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 100,
                          NULL);
    how = NET_PRESENT_FRESH;
    expect_state("late URGENT is SAFE",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_LATE, 200, &how),
                 NET_SAFE);
    expect_present("late URGENT is not fresh", how, NET_PRESENT_LATE);

    machine = machine_at(0, 3);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 100,
                          NULL);
    expect_state("silence after the pulse is SAFE",
                 net_machine_on_tick(&machine, 1101), NET_SAFE);

    machine = machine_at(0, 3);
    expect_state("enter SAFE from silence", net_machine_on_tick(&machine, 1001),
                 NET_SAFE);
    how = NET_PRESENT_FRESH;
    expect_state("late URGENT does not leave SAFE",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_LATE, 1500, &how),
                 NET_SAFE);
    expect_present("late URGENT while SAFE is not fresh", how, NET_PRESENT_LATE);
    how = NET_PRESENT_FRESH;
    expect_state("on-time COMMAND does not leave SAFE",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_COMMAND,
                                       SIGNAL_ON_TIME, 1600, &how),
                 NET_SAFE);
    expect_present("on-time COMMAND while SAFE is held", how, NET_PRESENT_HELD);
    how = NET_PRESENT_LATE;
    expect_state("first on-time URGENT enters RECOVERING",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_ON_TIME, 1700, &how),
                 NET_RECOVERING);
    expect_present("first recovery URGENT is held", how, NET_PRESENT_HELD);
    how = NET_PRESENT_FRESH;
    expect_state("second on-time URGENT stays RECOVERING",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_ON_TIME, 1800, &how),
                 NET_RECOVERING);
    expect_present("second recovery URGENT is held", how, NET_PRESENT_HELD);
    how = NET_PRESENT_HELD;
    expect_state("third on-time URGENT returns to NORMAL",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_ON_TIME, 1900, &how),
                 NET_NORMAL);
    expect_present("third recovery URGENT is fresh", how, NET_PRESENT_FRESH);

    machine = machine_at(0, 3);
    net_machine_on_tick(&machine, 1001);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 1100,
                          NULL);
    expect_state("RECOVERING", machine.state, NET_RECOVERING);
    expect_state("late URGENT during recovery returns to SAFE",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_LATE, 1200, NULL),
                 NET_SAFE);
    expect_state("streak starts over",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_ON_TIME, 1300, NULL),
                 NET_RECOVERING);

    machine = machine_at(0, 3);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 100,
                          NULL);
    expect_state("a dropped COMMAND is DEGRADED",
                 net_machine_on_dropped(&machine, SIGNAL_CLASS_COMMAND),
                 NET_DEGRADED);
    expect_state("a dropped URGENT is SAFE",
                 net_machine_on_dropped(&machine, SIGNAL_CLASS_URGENT), NET_SAFE);

    machine = machine_at(0, 3);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 100,
                          NULL);
    expect_state("lock", net_machine_on_lock(&machine), NET_LOCKED_SAFE);
    how = NET_PRESENT_FRESH;
    expect_state("on-time URGENT does not leave LOCKED_SAFE",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_ON_TIME, 200, &how),
                 NET_LOCKED_SAFE);
    expect_present("on-time URGENT while locked is held", how, NET_PRESENT_HELD);
    how = NET_PRESENT_FRESH;
    expect_state("late signal stays LOCKED_SAFE",
                 net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT,
                                       SIGNAL_LATE, 300, &how),
                 NET_LOCKED_SAFE);
    expect_present("late signal while locked is not fresh", how, NET_PRESENT_LATE);
    expect_state("silence does not leave LOCKED_SAFE",
                 net_machine_on_tick(&machine, 5000), NET_LOCKED_SAFE);

    machine = machine_at(0, 3);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_LATE, 100, NULL);
    expect_state("late URGENT does not refresh the pulse", machine.state, NET_SAFE);
    expect_state("the original due time still fires",
                 net_machine_on_tick(&machine, 1001), NET_SAFE);
    net_machine_on_signal(&machine, SIGNAL_CLASS_URGENT, SIGNAL_ON_TIME, 2000,
                          NULL);
    expect_state("on-time URGENT arms a new due time", machine.state,
                 NET_RECOVERING);
    expect_state("tick on the new due instant stays",
                 net_machine_on_tick(&machine, 3000), NET_RECOVERING);
    expect_state("tick after the new due time is SAFE",
                 net_machine_on_tick(&machine, 3001), NET_SAFE);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
