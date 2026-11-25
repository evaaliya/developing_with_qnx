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
                  "person out\n"
                  "arm home\n"
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
                  "person out\n"
                  "arm home\n"
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
                  "person out\n"
                  "arm home\n"
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
                  "person out\n"
                  "arm home\n"
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
                  "person out\n"
                  "arm home\n"
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
                  "person out\n"
                  "arm home\n"
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
                  "person out\n"
                  "arm home\n"
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
                  "person out\n"
                  "arm home\n"
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

    /* A restarted source takes the dead source's mode (D-065). */
    {
        char line[256];
        unsigned long cap = 12288; /* SIGNAL_MANIFEST_CAP */

        source_ctl_init(&ctl, 1);
        expect_int("restore: flood comes back",
                   (int)source_ctl_restore_mode(&ctl, 1, 1, 1, 1200, cap,
                                                line, sizeof line),
                   SOURCE_RESTORE_ALL);
        expect_int("restore: flood is on", ctl.flood, 1);
        expect_int("restore: urgent stays on", ctl.urgent_on, 1);
        expect_text("restore: flood line", line,
                    "source: restored the mode of the dead source: flood");

        source_ctl_init(&ctl, 1);
        expect_int("restore: mute comes back",
                   (int)source_ctl_restore_mode(&ctl, 1, 0, 0, 0, cap,
                                                line, sizeof line),
                   SOURCE_RESTORE_ALL);
        expect_int("restore: urgent is off", ctl.urgent_on, 0);
        expect_text("restore: mute line", line,
                    "source: restored the mode of the dead source: mute");

        source_ctl_init(&ctl, 1);
        (void)source_ctl_restore_mode(&ctl, 1, 1, 0, 10, cap, line, sizeof line);
        expect_text("restore: flood+mute line", line,
                    "source: restored the mode of the dead source: flood+mute");

        source_ctl_init(&ctl, 1);
        expect_int("restore: normal comes back as normal",
                   (int)source_ctl_restore_mode(&ctl, 1, 0, 1, 0, cap,
                                                line, sizeof line),
                   SOURCE_RESTORE_ALL);
        expect_text("restore: normal line", line,
                    "source: restored the mode of the dead source: normal");

        /* No room: the flood would stop itself at once. */
        source_ctl_init(&ctl, 1);
        expect_int("restore: no flood room keeps flood off",
                   (int)source_ctl_restore_mode(&ctl, 1, 1, 0,
                                                cap - SOURCE_FLOOD_RESERVE, cap,
                                                line, sizeof line),
                   SOURCE_RESTORE_NO_FLOOD);
        expect_int("restore: flood off without room", ctl.flood, 0);
        expect_int("restore: mute still comes back", ctl.urgent_on, 0);
        expect_text("restore: no room line", line,
                    "source: mode of the dead source was flood+mute; flood "
                    "stays off: 10240 of 12288 issued numbers wait to be "
                    "closed and 2048 are kept for the live schedule; mode is "
                    "mute");

        /* One number of room is enough. */
        source_ctl_init(&ctl, 1);
        expect_int("restore: one free number lets flood back",
                   (int)source_ctl_restore_mode(&ctl, 1, 1, 1,
                                                cap - SOURCE_FLOOD_RESERVE - 1,
                                                cap, line, sizeof line),
                   SOURCE_RESTORE_ALL);

        source_ctl_init(&ctl, 1);
        ctl.flood = 1;
        expect_int("restore: unknown mode",
                   (int)source_ctl_restore_mode(&ctl, 0, 1, 0, 0, cap,
                                                line, sizeof line),
                   SOURCE_RESTORE_UNKNOWN);
        expect_int("restore: unknown is normal (flood off)", ctl.flood, 0);
        expect_int("restore: unknown is normal (urgent on)", ctl.urgent_on, 1);
        expect_text("restore: unknown line", line,
                    "source: the book of the dead source holds no mode; mode "
                    "is normal");

        source_ctl_init(&ctl, 1);
        ctl.stop = 1;
        ctl.issued_urgent = 7;
        (void)source_ctl_restore_mode(&ctl, 1, 1, 1, 0, cap, NULL, 0);
        expect_int("restore: stop is not touched", ctl.stop, 1);
        expect_int("restore: counts are not touched", (int)ctl.issued_urgent, 7);
        expect_int("restore: NULL ctl",
                   (int)source_ctl_restore_mode(NULL, 1, 1, 1, 0, cap, NULL, 0),
                   SOURCE_RESTORE_UNKNOWN);
    }

    /* A send stuck in REPLY that the time limit cannot free (F-033). */
    expect_int("stuck: no send in progress",
               source_send_stuck(0, 9000000000LL, 6000000000LL), 0);
    expect_int("stuck: a send 5 s old is not stuck",
               source_send_stuck(1000000000LL, 6000000000LL, 6000000000LL), 0);
    expect_int("stuck: exactly at the limit is not stuck",
               source_send_stuck(1000000000LL, 7000000000LL, 6000000000LL), 0);
    expect_int("stuck: past the limit",
               source_send_stuck(1000000000LL, 7000000001LL, 6000000000LL), 1);
    expect_int("stuck: clock before the send start",
               source_send_stuck(5000000000LL, 1000000000LL, 6000000000LL), 0);
    expect_int("stuck: negative limit",
               source_send_stuck(1000000000LL, 9000000000LL, -1), 0);

    /* D-081: the next time is the previous time plus the period. */
    {
        int64_t nx[3] = { 0, 0, 0 };
        int64_t now;
        int issued = 0;
        int64_t last = 0;
        int64_t gap_max = 0;
        int64_t gap_min = INT64_MAX;
        int tick;

        source_ctl_init(&ctl, 1);
        /* Ticks 10 ms apart, each a little late by a varying amount. */
        for (tick = 0; tick < 500; tick++) {
            uint32_t got;

            now = (int64_t)tick * SOURCE_TICK_NS + (int64_t)((tick * 37) % 900) * 1000;
            while ((got = source_schedule_take(&ctl, now, nx)) != 0) {
                if (got == SIGNAL_CLASS_URGENT) {
                    if (issued > 0) {
                        int64_t g = now - last;

                        if (g > gap_max) {
                            gap_max = g;
                        }
                        if (g < gap_min) {
                            gap_min = g;
                        }
                    }
                    last = now;
                    issued++;
                }
            }
        }
        /* 5 s of ticks: one URGENT every 20 ms, none every 30 ms. */
        expect_int("drift: 250 URGENT in 500 ticks", issued, 250);
        expect_int("drift: no gap of three ticks", gap_max < 25000000LL, 1);
        expect_int("drift: no gap of one tick", gap_min > 15000000LL, 1);

        nx[0] = 100000000LL;
        expect_u32("due half a tick early",
                   source_schedule_take(&ctl, 100000000LL - SOURCE_TICK_NS / 2,
                                        nx), SIGNAL_CLASS_URGENT);
        expect_int("next is previous plus period", nx[0] == 120000000LL, 1);
        expect_u32("not due more than half a tick early",
                   source_schedule_take(&ctl, 120000000LL - SOURCE_TICK_NS / 2 - 1,
                                        nx), 0);
        nx[0] = 100000000LL;
        nx[1] = INT64_MAX;
        nx[2] = INT64_MAX;
        expect_u32("late by many periods is one signal",
                   source_schedule_take(&ctl, 172000000LL, nx),
                   SIGNAL_CLASS_URGENT);
        expect_int("missed times are skipped, phase kept", nx[0] == 180000000LL,
                   1);
        expect_u32("no burst after the skip",
                   source_schedule_take(&ctl, 172000000LL, nx), 0);
        source_schedule_restart(SIGNAL_CLASS_COMMAND, 1000, nx);
        expect_int("restart command schedule",
                   nx[1] == 1000 + SOURCE_COMMAND_PERIOD_NS, 1);
    }

    /* D-080: the zone sensor and the arm plan. */
    {
        struct signal_arm arm;

        source_ctl_init(&ctl, 1);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_URGENT, &arm);
        expect_u32("nobody in the zone", arm.flags, 0);
        expect_int("person in", source_ctl_apply_at(&ctl, "person in", 777), 0);
        expect_u32("person in wakes URGENT", ctl.wake, SOURCE_WAKE_URGENT);
        expect_int("event time kept", ctl.person_event_ns == 777, 1);
        ctl.wake = 0;
        expect_int("person in again", source_ctl_apply_at(&ctl, "person in", 999),
                   0);
        expect_u32("a repeat wakes nothing", ctl.wake, 0);
        expect_int("a repeat keeps the first event", ctl.person_event_ns == 777,
                   1);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_URGENT, &arm);
        expect_u32("URGENT says person in", arm.flags, SIGNAL_ARM_PERSON_IN);
        expect_int("URGENT carries the event", arm.event_ns == 777, 1);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_BACKGROUND, &arm);
        expect_u32("BACKGROUND carries nothing", arm.flags, 0);
        expect_int("person out", source_ctl_apply_at(&ctl, "person out\n", 1500),
                   0);
        expect_u32("person out wakes URGENT", ctl.wake, SOURCE_WAKE_URGENT);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_URGENT, &arm);
        expect_u32("URGENT says nobody", arm.flags, 0);
        ctl.wake = 0;
        expect_int("person out again", source_ctl_apply(&ctl, "person out"), 0);
        expect_u32("out again wakes nothing", ctl.wake, 0);

        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_COMMAND, &arm);
        expect_u32("COMMAND has a target", arm.flags, SIGNAL_ARM_TARGET);
        expect_int("default plan is home", arm.joint_mrad[0], -100);
        expect_int("arm eye", source_ctl_apply(&ctl, "arm eye"), 0);
        expect_u32("arm eye wakes COMMAND", ctl.wake, SOURCE_WAKE_COMMAND);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_COMMAND, &arm);
        expect_int("eye joint 1", arm.joint_mrad[0], 300);
        expect_int("eye joint 2", arm.joint_mrad[1], 1200);
        expect_int("eye joint 3", arm.joint_mrad[2], 500);
        expect_int("arm cycle", source_ctl_apply(&ctl, "arm cycle"), 0);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_COMMAND, &arm);
        expect_int("cycle starts at the eye", arm.joint_mrad[1], 1200);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_COMMAND, &arm);
        expect_int("cycle then home", arm.joint_mrad[1], 1600);
        source_ctl_arm_payload(&ctl, SIGNAL_CLASS_COMMAND, &arm);
        expect_int("cycle then eye", arm.joint_mrad[1], 1200);
        expect_int("unknown arm command", source_ctl_apply(&ctl, "arm up"), -1);
        source_ctl_arm_payload(NULL, SIGNAL_CLASS_COMMAND, &arm);
        expect_u32("NULL ctl carries nothing", arm.flags, 0);

        source_ctl_init(&ctl, 0);
        expect_int("burst ignores person in", source_ctl_apply(&ctl, "person in"),
                   -1);
        expect_int("burst person stays out", ctl.person_in, 0);
    }

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
