#include "source_ctl.h"

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

static void expect_text(const char *name, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s\n got: %s\n want: %s\n", name,
           got != NULL ? got : "(null)", want != NULL ? want : "(null)");
    failures++;
}

static void expect_status(const char *name, const struct source_control *ctl,
                          const char *want)
{
    char buf[512];
    int n;

    n = source_ctl_status(ctl, buf, sizeof buf);
    expect_int(name, n, (int)strlen(want));
    expect_text(name, buf, want);
}

int main(void)
{
    struct source_control ctl;
    int64_t next_ns[3];
    char buf[32];
    char long_line[80];

    source_ctl_init(&ctl, 1);
    expect_int("live starts in normal", ctl.flood, 0);
    expect_int("live starts with urgent", ctl.urgent_on, 1);
    expect_int("live starts without stop", ctl.stop, 0);
    expect_status("initial live status", &ctl,
                  "emission live\n"
                  "mode normal\n"
                  "flood off\n"
                  "urgent on\n"
                  "stop no\n"
                  "issued URGENT 0\n"
                  "issued COMMAND 0\n"
                  "issued BACKGROUND 0\n"
                  "error none\n");

    expect_int("flood on", source_ctl_apply(&ctl, "flood on\n"), 0);
    expect_int("flood flag", ctl.flood, 1);
    expect_status("flood status", &ctl,
                  "emission live\n"
                  "mode flood\n"
                  "flood on\n"
                  "urgent on\n"
                  "stop no\n"
                  "issued URGENT 0\n"
                  "issued COMMAND 0\n"
                  "issued BACKGROUND 0\n"
                  "error none\n");

    expect_int("flood on again", source_ctl_apply(&ctl, "  flood on  "), 0);
    expect_int("flood off", source_ctl_apply(&ctl, "flood off\r\n"), 0);
    expect_int("flood cleared", ctl.flood, 0);

    expect_int("mute", source_ctl_apply(&ctl, "mute"), 0);
    expect_int("urgent stopped", ctl.urgent_on, 0);
    expect_status("mute status", &ctl,
                  "emission live\n"
                  "mode mute\n"
                  "flood off\n"
                  "urgent off\n"
                  "stop no\n"
                  "issued URGENT 0\n"
                  "issued COMMAND 0\n"
                  "issued BACKGROUND 0\n"
                  "error none\n");

    expect_int("restore", source_ctl_apply(&ctl, "restore\n"), 0);
    expect_int("urgent returned", ctl.urgent_on, 1);
    expect_int("urgent off words", source_ctl_apply(&ctl, "urgent off"), 0);
    expect_int("urgent on words", source_ctl_apply(&ctl, "urgent on"), 0);
    expect_int("urgent is on", ctl.urgent_on, 1);

    expect_int("flood and mute together", source_ctl_apply(&ctl, "flood on"), 0);
    expect_int("then mute", source_ctl_apply(&ctl, "urgent off"), 0);
    expect_status("flood and mute status", &ctl,
                  "emission live\n"
                  "mode flood+mute\n"
                  "flood on\n"
                  "urgent off\n"
                  "stop no\n"
                  "issued URGENT 0\n"
                  "issued COMMAND 0\n"
                  "issued BACKGROUND 0\n"
                  "error none\n");
    expect_int("restore during flood", source_ctl_apply(&ctl, "restore"), 0);
    expect_int("flood off after restore", source_ctl_apply(&ctl, "flood off"), 0);

    source_ctl_note_issued(&ctl, SIGNAL_CLASS_URGENT);
    source_ctl_note_issued(&ctl, SIGNAL_CLASS_URGENT);
    source_ctl_note_issued(&ctl, SIGNAL_CLASS_COMMAND);
    source_ctl_note_issued(&ctl, SIGNAL_CLASS_BACKGROUND);
    source_ctl_note_issued(&ctl, SIGNAL_CLASS_BACKGROUND);
    source_ctl_note_issued(&ctl, SIGNAL_CLASS_BACKGROUND);
    expect_status("counters", &ctl,
                  "emission live\n"
                  "mode normal\n"
                  "flood off\n"
                  "urgent on\n"
                  "stop no\n"
                  "issued URGENT 2\n"
                  "issued COMMAND 1\n"
                  "issued BACKGROUND 3\n"
                  "error none\n");

    expect_int("unknown command", source_ctl_apply(&ctl, "nope\n"), -1);
    expect_int("unknown leaves flood off", ctl.flood, 0);
    expect_int("unknown leaves urgent on", ctl.urgent_on, 1);
    expect_int("unknown leaves the counters", (int)ctl.issued_urgent, 2);
    expect_status("unknown status", &ctl,
                  "emission live\n"
                  "mode normal\n"
                  "flood off\n"
                  "urgent on\n"
                  "stop no\n"
                  "issued URGENT 2\n"
                  "issued COMMAND 1\n"
                  "issued BACKGROUND 3\n"
                  "error unknown command\n");

    expect_int("empty command", source_ctl_apply(&ctl, " \n"), -1);
    expect_status("empty is unknown", &ctl,
                  "emission live\n"
                  "mode normal\n"
                  "flood off\n"
                  "urgent on\n"
                  "stop no\n"
                  "issued URGENT 2\n"
                  "issued COMMAND 1\n"
                  "issued BACKGROUND 3\n"
                  "error unknown command\n");

    memset(long_line, 'a', sizeof long_line - 1);
    long_line[sizeof long_line - 1] = '\0';
    expect_int("long command", source_ctl_apply(&ctl, long_line), -1);
    expect_int("long leaves urgent on", ctl.urgent_on, 1);
    expect_text("long error", ctl.error, "command too long");

    expect_int("a good command clears the error",
               source_ctl_apply(&ctl, "flood off"), 0);
    expect_text("error cleared", ctl.error, "");

    expect_int("stop", source_ctl_apply(&ctl, "stop\n"), 0);
    expect_int("stop flag", ctl.stop, 1);
    expect_int("stop again", source_ctl_apply(&ctl, "stop"), 0);

    source_ctl_init(&ctl, 0);
    expect_int("burst rejects flood", source_ctl_apply(&ctl, "flood on\n"), -1);
    expect_int("burst flood stays off", ctl.flood, 0);
    expect_int("burst rejects mute", source_ctl_apply(&ctl, "urgent off"), -1);
    expect_int("burst urgent stays on", ctl.urgent_on, 1);
    expect_text("burst error", ctl.error, "counted burst ignores commands");
    expect_status("burst status", &ctl,
                  "emission burst\n"
                  "mode normal\n"
                  "flood off\n"
                  "urgent on\n"
                  "stop no\n"
                  "issued URGENT 0\n"
                  "issued COMMAND 0\n"
                  "issued BACKGROUND 0\n"
                  "error counted burst ignores commands\n");

    expect_int("null control", source_ctl_apply(NULL, "stop"), -1);
    expect_int("status needs a buffer", source_ctl_status(&ctl, NULL, 8), -1);
    expect_int("status needs room", source_ctl_status(&ctl, buf, sizeof buf), -1);

    source_ctl_init(&ctl, 1);
    next_ns[0] = 0;
    next_ns[1] = 0;
    next_ns[2] = 0;
    expect_u32("first due is urgent",
               source_schedule_take(&ctl, 0, next_ns), SIGNAL_CLASS_URGENT);
    expect_u32("then command",
               source_schedule_take(&ctl, 0, next_ns), SIGNAL_CLASS_COMMAND);
    expect_u32("then background",
               source_schedule_take(&ctl, 0, next_ns), SIGNAL_CLASS_BACKGROUND);
    expect_u32("then nothing", source_schedule_take(&ctl, 0, next_ns), 0);
    expect_int("urgent next", next_ns[0] == SOURCE_URGENT_PERIOD_NS, 1);
    expect_int("command next", next_ns[1] == SOURCE_COMMAND_PERIOD_NS, 1);
    expect_int("background next", next_ns[2] == SOURCE_BACKGROUND_PERIOD_NS, 1);

    expect_u32("at the urgent period",
               source_schedule_take(&ctl, SOURCE_URGENT_PERIOD_NS, next_ns),
               SIGNAL_CLASS_URGENT);
    expect_u32("background waits for its own period",
               source_schedule_take(&ctl, SOURCE_URGENT_PERIOD_NS, next_ns), 0);
    expect_u32("command is not due yet",
               source_schedule_take(&ctl, SOURCE_URGENT_PERIOD_NS, next_ns), 0);
    expect_u32("urgent is due again on the background period",
               source_schedule_take(&ctl, SOURCE_BACKGROUND_PERIOD_NS, next_ns),
               SIGNAL_CLASS_URGENT);
    expect_u32("background is due on its period",
               source_schedule_take(&ctl, SOURCE_BACKGROUND_PERIOD_NS, next_ns),
               SIGNAL_CLASS_BACKGROUND);

    next_ns[0] = 0;
    expect_int("mute the schedule", source_ctl_apply(&ctl, "mute"), 0);
    expect_u32("muted urgent is skipped",
               source_schedule_take(&ctl, SOURCE_URGENT_PERIOD_NS, next_ns), 0);
    expect_int("muted urgent time stays due", next_ns[0] == 0, 1);
    expect_int("restore the schedule", source_ctl_apply(&ctl, "restore"), 0);
    expect_u32("restored urgent is due at once",
               source_schedule_take(&ctl, SOURCE_URGENT_PERIOD_NS, next_ns),
               SIGNAL_CLASS_URGENT);

    {
        int64_t flood_next = 0;

        expect_u32("no flood extras", source_flood_due(0, 0, &flood_next), 0);
        expect_int("off does not arm the next flood", flood_next == 0, 1);
        expect_u32("flood batch is due at once",
                   source_flood_due(1, 0, &flood_next), SOURCE_FLOOD_BATCH);
        expect_int("flood period is armed",
                   flood_next == SOURCE_FLOOD_PERIOD_NS, 1);
        expect_u32("flood waits out its period",
                   source_flood_due(1, SOURCE_FLOOD_PERIOD_NS - 1, &flood_next),
                   0);
        expect_u32("flood is due again",
                   source_flood_due(1, SOURCE_FLOOD_PERIOD_NS, &flood_next),
                   SOURCE_FLOOD_BATCH);
        expect_u32("null flood clock", source_flood_due(1, 0, NULL), 0);
        flood_next = 0;
        expect_u32("the next flood is a full batch again",
                   source_flood_due(1, 0, &flood_next), SOURCE_FLOOD_BATCH);
    }
    expect_u32("null schedule", source_schedule_take(NULL, 0, next_ns), 0);

    {
        const unsigned long cap = 12288;
        const unsigned long limit = cap - SOURCE_FLOOD_RESERVE;

        expect_u32("flood room on an empty list",
                   source_flood_room(0, cap, SOURCE_FLOOD_BATCH),
                   SOURCE_FLOOD_BATCH);
        expect_u32("flood room short of the reserve",
                   source_flood_room(limit - 10, cap, SOURCE_FLOOD_BATCH), 10);
        expect_u32("no flood room at the reserve",
                   source_flood_room(limit, cap, SOURCE_FLOOD_BATCH), 0);
        expect_u32("no flood room inside the reserve",
                   source_flood_room(cap - 1, cap, SOURCE_FLOOD_BATCH), 0);
        expect_u32("no flood room past the cap",
                   source_flood_room(cap + 5, cap, SOURCE_FLOOD_BATCH), 0);
        expect_u32("no flood room when the cap is the reserve",
                   source_flood_room(0, SOURCE_FLOOD_RESERVE,
                                     SOURCE_FLOOD_BATCH), 0);
        expect_u32("no flood room when none is wanted",
                   source_flood_room(0, cap, 0), 0);
    }

    source_ctl_init(&ctl, 1);
    expect_int("flood on is accepted again after the flood stopped itself",
               source_ctl_apply(&ctl, "flood on"), 0);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
