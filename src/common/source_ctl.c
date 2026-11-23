#include "source_ctl.h"

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
                     "issued URGENT %llu\n"
                     "issued COMMAND %llu\n"
                     "issued BACKGROUND %llu\n"
                     "error %s\n",
                     ctl->live ? "live" : "burst",
                     mode,
                     ctl->flood ? "on" : "off",
                     ctl->urgent_on ? "on" : "off",
                     ctl->stop ? "yes" : "no",
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
        if (now_ns < next_ns[index]) {
            continue;
        }
        next_ns[index] = add_period(now_ns, period_of(class_id));
        return class_id;
    }
    return 0;
}

unsigned source_flood_due(int flood, int64_t now_ns, int64_t *next_ns,
                          unsigned already)
{
    unsigned burst;
    unsigned room;

    if (!flood || next_ns == NULL || already >= SOURCE_FLOOD_CAP) {
        return 0;
    }
    if (now_ns < *next_ns) {
        return 0;
    }
    *next_ns = add_period(now_ns, SOURCE_FLOOD_PERIOD_NS);
    burst = already == 0 ? SOURCE_FLOOD_FIRST : SOURCE_FLOOD_BURST;
    room = SOURCE_FLOOD_CAP - already;
    if (room > burst) {
        return burst;
    }
    return room;
}
