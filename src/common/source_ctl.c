#include "source_ctl.h"

#include "arm_model.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void source_ctl_init(struct source_control *ctl, int live)
{
    if (ctl == NULL) {
        return;
    }
    memset(ctl, 0, sizeof *ctl);
    ctl->live = live ? 1 : 0;
    ctl->urgent_on = 1;
}

static int copy_command(const char *text, char *out, size_t cap, int *empty)
{
    const char *start;
    const char *end;
    size_t n;

    *empty = 0;
    if (text == NULL) {
        *empty = 1;
        return -1;
    }
    start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    end = start + strlen(start);
    while (end > start &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
            end[-1] == '\n')) {
        end--;
    }
    n = (size_t)(end - start);
    if (n == 0) {
        *empty = 1;
        return -1;
    }
    if (n >= cap) {
        return -1;
    }
    memcpy(out, start, n);
    out[n] = '\0';
    return 0;
}

int source_ctl_apply(struct source_control *ctl, const char *text)
{
    return source_ctl_apply_at(ctl, text, 0);
}

int source_ctl_apply_at(struct source_control *ctl, const char *text,
                        int64_t now_ns)
{
    char cmd[64];
    int empty = 0;

    if (ctl == NULL) {
        return -1;
    }
    if (copy_command(text, cmd, sizeof cmd, &empty) != 0) {
        snprintf(ctl->error, sizeof ctl->error,
                 empty ? "unknown command" : "command too long");
        return -1;
    }
    if (!ctl->live) {
        snprintf(ctl->error, sizeof ctl->error, "counted burst ignores commands");
        return -1;
    }
    if (strcmp(cmd, "flood on") == 0) {
        ctl->flood = 1;
    } else if (strcmp(cmd, "flood off") == 0) {
        ctl->flood = 0;
    } else if (strcmp(cmd, "urgent off") == 0 || strcmp(cmd, "mute") == 0) {
        ctl->urgent_on = 0;
    } else if (strcmp(cmd, "urgent on") == 0 || strcmp(cmd, "restore") == 0) {
        ctl->urgent_on = 1;
    } else if (strcmp(cmd, "stop") == 0) {
        ctl->stop = 1;
    } else if (strcmp(cmd, "person in") == 0) {
        if (!ctl->person_in) {
            ctl->person_in = 1;
            ctl->person_event_ns = now_ns;
            ctl->wake |= SOURCE_WAKE_URGENT;
        }
    } else if (strcmp(cmd, "person out") == 0) {
        if (ctl->person_in) {
            ctl->person_in = 0;
            ctl->person_event_ns = now_ns;
            ctl->wake |= SOURCE_WAKE_URGENT;
        }
    } else if (strcmp(cmd, "arm eye") == 0) {
        ctl->arm_plan = SOURCE_ARM_EYE;
        ctl->wake |= SOURCE_WAKE_COMMAND;
    } else if (strcmp(cmd, "arm home") == 0) {
        ctl->arm_plan = SOURCE_ARM_HOME;
        ctl->wake |= SOURCE_WAKE_COMMAND;
    } else if (strcmp(cmd, "arm cycle") == 0) {
        ctl->arm_plan = SOURCE_ARM_CYCLE;
        ctl->cycle_to_eye = 1;
        ctl->wake |= SOURCE_WAKE_COMMAND;
    } else {
        snprintf(ctl->error, sizeof ctl->error, "unknown command");
        return -1;
    }
    ctl->error[0] = '\0';
    return 0;
}

void source_ctl_note_issued(struct source_control *ctl, uint32_t class_id)
{
    if (ctl == NULL) {
        return;
    }
    if (class_id == SIGNAL_CLASS_URGENT) {
        ctl->issued_urgent++;
    } else if (class_id == SIGNAL_CLASS_COMMAND) {
        ctl->issued_command++;
    } else if (class_id == SIGNAL_CLASS_BACKGROUND) {
        ctl->issued_background++;
    }
}

int source_ctl_status(const struct source_control *ctl, char *buf, size_t cap)
{
    const char *mode;
    int wrote;

    if (ctl == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    if (ctl->flood && !ctl->urgent_on) {
        mode = "flood+mute";
    } else if (ctl->flood) {
        mode = "flood";
    } else if (!ctl->urgent_on) {
        mode = "mute";
    } else {
        mode = "normal";
    }
    wrote = snprintf(buf, cap,
                     "emission %s\n"
                     "mode %s\n"
                     "flood %s\n"
                     "urgent %s\n"
                     "stop %s\n"
                     "person %s\n"
                     "arm %s\n"
                     "issued URGENT %llu\n"
                     "issued COMMAND %llu\n"
                     "issued BACKGROUND %llu\n"
                     "error %s\n",
                     ctl->live ? "live" : "burst",
                     mode,
                     ctl->flood ? "on" : "off",
                     ctl->urgent_on ? "on" : "off",
                     ctl->stop ? "yes" : "no",
                     ctl->person_in ? "in" : "out",
                     ctl->arm_plan == SOURCE_ARM_EYE     ? "eye"
                     : ctl->arm_plan == SOURCE_ARM_CYCLE ? "cycle"
                                                         : "home",
                     (unsigned long long)ctl->issued_urgent,
                     (unsigned long long)ctl->issued_command,
                     (unsigned long long)ctl->issued_background,
                     ctl->error[0] != '\0' ? ctl->error : "none");
    if (wrote < 0 || (size_t)wrote >= cap) {
        return -1;
    }
    return wrote;
}

static int64_t add_period(int64_t now_ns, int64_t period_ns)
{
    if (period_ns < 0) {
        return now_ns;
    }
    if (now_ns > INT64_MAX - period_ns) {
        return INT64_MAX;
    }
    return now_ns + period_ns;
}

static int64_t period_of(uint32_t class_id)
{
    switch (class_id) {
    case SIGNAL_CLASS_URGENT:
        return SOURCE_URGENT_PERIOD_NS;
    case SIGNAL_CLASS_COMMAND:
        return SOURCE_COMMAND_PERIOD_NS;
    case SIGNAL_CLASS_BACKGROUND:
        return SOURCE_BACKGROUND_PERIOD_NS;
    default:
        return 0;
    }
}

static int32_t to_mrad(double rad)
{
    double m = rad * 1000.0;

    return (int32_t)(m < 0.0 ? m - 0.5 : m + 0.5);
}

void source_ctl_arm_payload(struct source_control *ctl, uint32_t class_id,
                            struct signal_arm *out)
{
    int to_eye;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (ctl == NULL) {
        return;
    }
    if (class_id == SIGNAL_CLASS_URGENT) {
        if (ctl->person_in) {
            out->flags = SIGNAL_ARM_PERSON_IN;
        }
        out->event_ns = ctl->person_event_ns;
        return;
    }
    if (class_id != SIGNAL_CLASS_COMMAND) {
        return;
    }
    if (ctl->arm_plan == SOURCE_ARM_CYCLE) {
        to_eye = ctl->cycle_to_eye;
        ctl->cycle_to_eye = !ctl->cycle_to_eye;
    } else {
        to_eye = ctl->arm_plan == SOURCE_ARM_EYE;
    }
    out->flags = SIGNAL_ARM_TARGET;
    if (to_eye) {
        out->joint_mrad[0] = to_mrad(ARM_EYE_Q1);
        out->joint_mrad[1] = to_mrad(ARM_EYE_Q2);
        out->joint_mrad[2] = to_mrad(ARM_EYE_Q3);
    } else {
        out->joint_mrad[0] = to_mrad(ARM_HOME_Q1);
        out->joint_mrad[1] = to_mrad(ARM_HOME_Q2);
        out->joint_mrad[2] = to_mrad(ARM_HOME_Q3);
    }
}

void source_schedule_restart(uint32_t class_id, int64_t now_ns,
                             int64_t next_ns[3])
{
    int64_t period = period_of(class_id);

    if (next_ns == NULL || period <= 0) {
        return;
    }
    next_ns[class_id - 1] = add_period(now_ns, period);
}

/*
 * The next time after a class was taken at now_ns: the previous time
 * plus whole periods, the first one after now (D-081).
 */
static int64_t next_due(int64_t prev_ns, int64_t now_ns, int64_t period)
{
    int64_t next;

    if (prev_ns <= 0 || period <= 0) {
        return add_period(now_ns, period);
    }
    next = add_period(prev_ns, period);
    if (next <= now_ns) {
        int64_t behind = now_ns - prev_ns;

        next = add_period(prev_ns, (behind / period + 1) * period);
    }
    return next;
}

uint32_t source_schedule_take(const struct source_control *ctl, int64_t now_ns,
                              int64_t next_ns[3])
{
    static const uint32_t order[3] = {
        SIGNAL_CLASS_URGENT,
        SIGNAL_CLASS_COMMAND,
        SIGNAL_CLASS_BACKGROUND
    };
    int i;

    if (ctl == NULL || next_ns == NULL) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        uint32_t class_id = order[i];
        int index = (int)class_id - 1;

        if (class_id == SIGNAL_CLASS_URGENT && !ctl->urgent_on) {
            continue;
        }
        if (now_ns + SOURCE_TICK_NS / 2 < next_ns[index]) {
            continue;
        }
        next_ns[index] = next_due(next_ns[index], now_ns, period_of(class_id));
        return class_id;
    }
    return 0;
}

unsigned source_flood_due(int flood, int64_t now_ns, int64_t *next_ns)
{
    if (!flood || next_ns == NULL) {
        return 0;
    }
    if (now_ns < *next_ns) {
        return 0;
    }
    *next_ns = add_period(now_ns, SOURCE_FLOOD_PERIOD_NS);
    return SOURCE_FLOOD_BATCH;
}

unsigned source_flood_room(unsigned long recorded, unsigned long cap,
                           unsigned want)
{
    unsigned long limit;
    unsigned long room;

    if (cap <= SOURCE_FLOOD_RESERVE) {
        return 0;
    }
    limit = cap - SOURCE_FLOOD_RESERVE;
    if (recorded >= limit) {
        return 0;
    }
    room = limit - recorded;
    return room < want ? (unsigned)room : want;
}

static const char *mode_name(int flood, int urgent_on)
{
    if (flood && !urgent_on) {
        return "flood+mute";
    }
    if (flood) {
        return "flood";
    }
    if (!urgent_on) {
        return "mute";
    }
    return "normal";
}

enum source_mode_restore source_ctl_restore_mode(struct source_control *ctl,
                                                 int known, int flood,
                                                 int urgent_on,
                                                 unsigned long waiting,
                                                 unsigned long cap,
                                                 char *line, size_t line_cap)
{
    char scratch[8];

    if (line == NULL || line_cap == 0) {
        line = scratch;
        line_cap = sizeof scratch;
    }
    if (ctl == NULL || !known) {
        if (ctl != NULL) {
            ctl->flood = 0;
            ctl->urgent_on = 1;
        }
        snprintf(line, line_cap,
                 "source: the book of the dead source holds no mode; "
                 "mode is normal");
        return SOURCE_RESTORE_UNKNOWN;
    }
    ctl->urgent_on = urgent_on ? 1 : 0;
    if (flood && source_flood_room(waiting, cap, 1) == 0) {
        ctl->flood = 0;
        snprintf(line, line_cap,
                 "source: mode of the dead source was %s; flood stays off: "
                 "%lu of %lu issued numbers wait to be closed and %u are kept "
                 "for the live schedule; mode is %s",
                 mode_name(1, ctl->urgent_on), waiting, cap,
                 (unsigned)SOURCE_FLOOD_RESERVE, mode_name(0, ctl->urgent_on));
        return SOURCE_RESTORE_NO_FLOOD;
    }
    ctl->flood = flood ? 1 : 0;
    snprintf(line, line_cap, "source: restored the mode of the dead source: %s",
             mode_name(ctl->flood, ctl->urgent_on));
    return SOURCE_RESTORE_ALL;
}

int source_send_stuck(int64_t since_ns, int64_t now_ns, int64_t limit_ns)
{
    if (since_ns <= 0 || limit_ns < 0 || now_ns < since_ns) {
        return 0;
    }
    return now_ns - since_ns > limit_ns;
}
