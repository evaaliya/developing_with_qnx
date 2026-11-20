#include "net_state.h"

#include <stddef.h>
#include <stdint.h>

static int64_t add_gap(int64_t now_ns, int64_t gap_ns)
{
    if (gap_ns < 0) {
        return now_ns;
    }
    if (now_ns > INT64_MAX - gap_ns) {
        return INT64_MAX;
    }
    return now_ns + gap_ns;
}

static void set_presentation(enum net_presentation *presentation,
                             enum net_presentation value)
{
    if (presentation != NULL) {
        *presentation = value;
    }
}

static int path_up(enum net_state state)
{
    return state == NET_NORMAL || state == NET_DEGRADED;
}

static enum net_state state_after_urgent_ok(struct net_machine *machine)
{
    if (machine->command_behind || machine->background_behind) {
        return NET_DEGRADED;
    }
    return NET_NORMAL;
}

void net_machine_init(struct net_machine *machine, int64_t start_ns,
                      int64_t urgent_gap_ns, int recover_need)
{
    if (machine == NULL) {
        return;
    }
    machine->state = NET_INIT;
    machine->recover_need = recover_need >= 1 ? recover_need : NET_RECOVER_URGENT_NEED;
    machine->recover_streak = 0;
    machine->urgent_gap_ns = urgent_gap_ns > 0 ? urgent_gap_ns : 0;
    machine->next_urgent_due_ns = add_gap(start_ns, machine->urgent_gap_ns);
    machine->command_behind = 0;
    machine->background_behind = 0;
    machine->locked = 0;
}

enum net_state net_machine_on_signal(struct net_machine *machine,
                                     uint32_t class_id,
                                     enum signal_verdict verdict,
                                     int64_t now_ns,
                                     enum net_presentation *presentation)
{
    int urgent;
    int command;
    int background;

    if (machine == NULL) {
        set_presentation(presentation, NET_PRESENT_LATE);
        return NET_SAFE;
    }

    urgent = class_id == SIGNAL_CLASS_URGENT;
    command = class_id == SIGNAL_CLASS_COMMAND;
    background = class_id == SIGNAL_CLASS_BACKGROUND;

    if (machine->locked) {
        set_presentation(presentation,
                         verdict == SIGNAL_LATE ? NET_PRESENT_LATE
                                                : NET_PRESENT_HELD);
        return machine->state;
    }

    if (verdict == SIGNAL_LATE) {
        set_presentation(presentation, NET_PRESENT_LATE);
        if (urgent) {
            machine->recover_streak = 0;
            machine->state = NET_SAFE;
            return machine->state;
        }
        if (command) {
            machine->command_behind = 1;
        } else if (background) {
            machine->background_behind = 1;
        }
        if (path_up(machine->state) && (command || background)) {
            machine->state = NET_DEGRADED;
        }
        return machine->state;
    }

    if (urgent) {
        machine->next_urgent_due_ns = add_gap(now_ns, machine->urgent_gap_ns);
        if (machine->state == NET_SAFE) {
            machine->recover_streak = 1;
            machine->state = NET_RECOVERING;
            set_presentation(presentation, NET_PRESENT_HELD);
            return machine->state;
        }
        if (machine->state == NET_RECOVERING) {
            machine->recover_streak++;
            if (machine->recover_streak >= machine->recover_need) {
                machine->state = state_after_urgent_ok(machine);
                set_presentation(presentation, NET_PRESENT_FRESH);
            } else {
                set_presentation(presentation, NET_PRESENT_HELD);
            }
            return machine->state;
        }
        if (machine->state == NET_INIT) {
            machine->state = state_after_urgent_ok(machine);
        }
        set_presentation(presentation,
                         path_up(machine->state) ? NET_PRESENT_FRESH
                                                 : NET_PRESENT_HELD);
        return machine->state;
    }

    if (command) {
        machine->command_behind = 0;
    } else if (background) {
        machine->background_behind = 0;
    }

    if (machine->state == NET_DEGRADED &&
        !machine->command_behind && !machine->background_behind) {
        machine->state = NET_NORMAL;
    }

    set_presentation(presentation,
                     path_up(machine->state) ? NET_PRESENT_FRESH
                                             : NET_PRESENT_HELD);
    return machine->state;
}

enum net_state net_machine_on_tick(struct net_machine *machine, int64_t now_ns)
{
    if (machine == NULL) {
        return NET_SAFE;
    }
    if (machine->locked) {
        return machine->state;
    }
    if (now_ns > machine->next_urgent_due_ns) {
        machine->recover_streak = 0;
        machine->state = NET_SAFE;
    }
    return machine->state;
}

enum net_state net_machine_on_dropped(struct net_machine *machine,
                                      uint32_t class_id)
{
    if (machine == NULL) {
        return NET_SAFE;
    }
    if (machine->locked) {
        return machine->state;
    }
    if (class_id == SIGNAL_CLASS_URGENT) {
        machine->recover_streak = 0;
        machine->state = NET_SAFE;
        return machine->state;
    }
    if (class_id == SIGNAL_CLASS_COMMAND) {
        machine->command_behind = 1;
    } else if (class_id == SIGNAL_CLASS_BACKGROUND) {
        machine->background_behind = 1;
    } else {
        return machine->state;
    }
    if (path_up(machine->state)) {
        machine->state = NET_DEGRADED;
    }
    return machine->state;
}

enum net_state net_machine_on_lock(struct net_machine *machine)
{
    if (machine == NULL) {
        return NET_LOCKED_SAFE;
    }
    machine->locked = 1;
    machine->recover_streak = 0;
    machine->state = NET_LOCKED_SAFE;
    return machine->state;
}

const char *net_state_name(enum net_state state)
{
    switch (state) {
    case NET_INIT:
        return "INIT";
    case NET_NORMAL:
        return "NORMAL";
    case NET_DEGRADED:
        return "DEGRADED";
    case NET_SAFE:
        return "SAFE";
    case NET_RECOVERING:
        return "RECOVERING";
    case NET_LOCKED_SAFE:
        return "LOCKED_SAFE";
    default:
        return "unknown";
    }
}

const char *net_presentation_name(enum net_presentation presentation)
{
    switch (presentation) {
    case NET_PRESENT_FRESH:
        return "fresh";
    case NET_PRESENT_LATE:
        return "late";
    case NET_PRESENT_HELD:
    default:
        return "held";
    }
}
