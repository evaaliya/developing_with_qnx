#include "signal_msg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(const char *name, enum signal_verdict got,
                   enum signal_verdict want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %s, want %s)\n", name, signal_verdict_name(got),
           signal_verdict_name(want));
    failures++;
}

static struct signal make_signal(int64_t issued_ns, int64_t deadline_ns)
{
    struct signal sig;

    sig.number = 1;
    sig.class_id = SIGNAL_CLASS_COMMAND;
    sig.hop_count = 0;
    sig.issued_ns = issued_ns;
    sig.deadline_ns = deadline_ns;
    memset(sig.hop_ns, 0, sizeof sig.hop_ns);
    return sig;
}

int main(void)
{
    struct signal sig;

    sig = make_signal(1000, 500);
    expect("on time", signal_check(&sig, 1499), SIGNAL_ON_TIME);

    sig = make_signal(1000, 500);
    expect("exactly on the boundary", signal_check(&sig, 1500), SIGNAL_ON_TIME);

    sig = make_signal(1000, 500);
    expect("late", signal_check(&sig, 1501), SIGNAL_LATE);

    sig = make_signal(1000, 0);
    expect("zero deadline, one nanosecond later", signal_check(&sig, 1001),
           SIGNAL_LATE);
    expect("zero deadline, at the issue instant", signal_check(&sig, 1000),
           SIGNAL_ON_TIME);

    if (signal_class_deadline_ns(SIGNAL_CLASS_URGENT) !=
            SIGNAL_DEADLINE_URGENT_NS ||
        signal_class_deadline_ns(SIGNAL_CLASS_COMMAND) !=
            SIGNAL_DEADLINE_COMMAND_NS ||
        signal_class_deadline_ns(SIGNAL_CLASS_BACKGROUND) !=
            SIGNAL_DEADLINE_BACKGROUND_NS ||
        signal_class_deadline_ns(0) != -1) {
        printf("FAIL class deadlines\n");
        failures++;
    } else if (!(SIGNAL_DEADLINE_URGENT_NS < SIGNAL_DEADLINE_COMMAND_NS &&
                 SIGNAL_DEADLINE_COMMAND_NS < SIGNAL_DEADLINE_BACKGROUND_NS &&
                 SIGNAL_DEADLINE_COMMAND_NS >= 2 * SIGNAL_DEADLINE_URGENT_NS &&
                 SIGNAL_DEADLINE_BACKGROUND_NS >=
                     2 * SIGNAL_DEADLINE_COMMAND_NS)) {
        printf("FAIL deadline order\n");
        failures++;
    } else {
        printf("PASS class deadlines\n");
    }

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
