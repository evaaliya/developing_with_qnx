#include "arm_drive.h"

#include <string.h>

void arm_link_init(struct arm_link *link)
{
    memset(link, 0, sizeof *link);
}

int arm_link_on_urgent(struct arm_link *link, const struct signal *sig,
                       int64_t now_ns)
{
    int in;
    int was;

    if (link == NULL || sig == NULL || sig->class_id != SIGNAL_CLASS_URGENT) {
        return 0;
    }
    if (sig->issued_ns < link->zone_issued_ns) {
        return 0;
    }
    link->zone_issued_ns = sig->issued_ns;
    in = (sig->arm.flags & SIGNAL_ARM_PERSON_IN) != 0;
    was = __atomic_load_n(&link->person_in, __ATOMIC_RELAXED);
    if (in == was) {
        return 0;
    }
    if (in) {
        /* The fields first, the flag last: the reader sees them whole. */
        __atomic_store_n(&link->person_since_ns, now_ns, __ATOMIC_RELAXED);
        __atomic_store_n(&link->event_ns, sig->arm.event_ns, __ATOMIC_RELAXED);
        __atomic_store_n(&link->stop_issued_ns, sig->issued_ns,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&link->stop_number, sig->number, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&link->person_in, in, __ATOMIC_RELEASE);
    return in;
}

void arm_link_on_command(struct arm_link *link, const struct signal *sig)
{
    uint32_t seq;
    int i;

    if (link == NULL || sig == NULL || sig->class_id != SIGNAL_CLASS_COMMAND ||
        (sig->arm.flags & SIGNAL_ARM_TARGET) == 0) {
        return;
    }
    if (sig->issued_ns < link->target_issued_ns) {
        return;
    }
    seq = __atomic_load_n(&link->target_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&link->target_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    link->target_issued_ns = sig->issued_ns;
    for (i = 0; i < ARM_JOINTS; i++) {
        __atomic_store_n(&link->target_mrad[i], sig->arm.joint_mrad[i],
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&link->target_seq, seq + 2, __ATOMIC_RELEASE);
}

void arm_link_on_state(struct arm_link *link, int ok, int64_t now_ns)
{
    if (link == NULL) {
        return;
    }
    ok = ok ? 1 : 0;
    if (__atomic_load_n(&link->state_ok, __ATOMIC_RELAXED) == ok) {
        return;
    }
    __atomic_store_n(&link->state_since_ns, now_ns, __ATOMIC_RELAXED);
    __atomic_store_n(&link->state_ok, ok, __ATOMIC_RELEASE);
}

void arm_link_read(const struct arm_link *link, struct arm_link_view *view)
{
    int tries;
    int i;

    memset(view, 0, sizeof *view);
    view->person_in = __atomic_load_n(&link->person_in, __ATOMIC_ACQUIRE);
    view->person_since_ns = __atomic_load_n(&link->person_since_ns,
                                            __ATOMIC_RELAXED);
    view->event_ns = __atomic_load_n(&link->event_ns, __ATOMIC_RELAXED);
    view->stop_issued_ns = __atomic_load_n(&link->stop_issued_ns,
                                           __ATOMIC_RELAXED);
    view->stop_number = __atomic_load_n(&link->stop_number, __ATOMIC_RELAXED);
    view->state_ok = __atomic_load_n(&link->state_ok, __ATOMIC_ACQUIRE);
    view->state_since_ns = __atomic_load_n(&link->state_since_ns,
                                           __ATOMIC_RELAXED);
    for (tries = 0; tries < 8; tries++) {
        uint32_t s0 = __atomic_load_n(&link->target_seq, __ATOMIC_ACQUIRE);
        int32_t m[ARM_JOINTS];

        if ((s0 & 1u) != 0) {
            continue;
        }
        for (i = 0; i < ARM_JOINTS; i++) {
            m[i] = __atomic_load_n(&link->target_mrad[i], __ATOMIC_RELAXED);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&link->target_seq, __ATOMIC_RELAXED) != s0) {
            continue;
        }
        view->target_seq = s0;
        for (i = 0; i < ARM_JOINTS; i++) {
            view->target[i] = (double)m[i] / 1000.0;
        }
        return;
    }
    /* A writer kept the target busy; this step keeps the old target. */
    view->target_seq = 0;
}

void arm_drive_init(struct arm_drive *drive, int64_t now_ns)
{
    double home[ARM_JOINTS];

    memset(drive, 0, sizeof *drive);
    arm_home_pose(home);
    arm_init(&drive->arm, home, now_ns);
    arm_brake(&drive->arm, now_ns);
    arm_step(&drive->arm, now_ns);
    drive->reason = ARM_BRAKE_STATE;
}

const char *arm_track_end_name(int end)
{
    switch (end) {
    case ARM_TRACK_STOP:
        return "stop";
    case ARM_TRACK_TOUCH:
        return "touch";
    case ARM_TRACK_FULL:
        return "full";
    default:
        return "open";
    }
}

const char *arm_brake_reason_name(int reason)
{
    switch (reason) {
    case ARM_BRAKE_PERSON:
        return "person";
    case ARM_BRAKE_STATE:
        return "state";
    default:
        return "none";
    }
}

static int32_t eye_um_now(const struct arm_state *arm)
{
    double um = arm_eye_distance(arm->q) * 1e6;

    return (int32_t)(um + 0.5);
}

static void track_end(struct arm_track *tr, int end)
{
    tr->active = 0;
    __atomic_store_n(&tr->end, end, __ATOMIC_RELEASE);
}

static void track_point(struct arm_drive *drive, int64_t t)
{
    struct arm_track *tr = &drive->track;
    double eye = arm_eye_distance(drive->arm.q);
    unsigned n = tr->count;

    if (n >= ARM_TRACK_CAP) {
        track_end(tr, ARM_TRACK_FULL);
        return;
    }
    tr->eye_um[n] = eye_um_now(&drive->arm);
    __atomic_store_n(&tr->count, n + 1, __ATOMIC_RELEASE);
    if (eye <= ARM_TOUCH_M && n > 0) {
        /* Between the last point and this one, at the last speed. */
        double prev_m = tr->eye_um[n - 1] / 1e6;
        int64_t at = t;

        if (tr->last_speed > 0.0) {
            int64_t dt = (int64_t)(prev_m / tr->last_speed * 1e9);

            if (dt >= 0 && dt < ARM_TRACK_STEP_NS) {
                at = t - ARM_TRACK_STEP_NS + dt;
            }
        }
        tr->touch_ns = at;
        track_end(tr, ARM_TRACK_TOUCH);
        return;
    }
    tr->last_speed = drive->arm.speed;
}

/* A new approach: the target changed and no track has ended yet. */
static void track_start(struct arm_drive *drive)
{
    struct arm_track *tr = &drive->track;

    if (__atomic_load_n(&tr->end, __ATOMIC_RELAXED) != ARM_TRACK_OPEN) {
        return;
    }
    __atomic_store_n(&tr->count, 0, __ATOMIC_RELAXED);
    tr->active = 1;
    tr->start_ns = drive->arm.t_ns;
    tr->next_ns = tr->start_ns + ARM_TRACK_STEP_NS;
    tr->last_speed = 0.0;
    tr->touch_ns = 0;
    tr->stop_number = 0;
    if (drive->arm.brake) {
        tr->brake_ns = drive->arm.brake_ns;
        tr->brake_by = (int)drive->reason;
    } else {
        tr->brake_ns = 0;
        tr->brake_by = ARM_BRAKE_NONE;
    }
    track_point(drive, tr->start_ns);
}

/* The brake engaged at at_ns for reason, during the track. */
static void track_brake(struct arm_drive *drive, int64_t at_ns, int reason,
                        uint64_t number)
{
    struct arm_track *tr = &drive->track;

    if (!tr->active || tr->brake_by != ARM_BRAKE_NONE) {
        return;
    }
    tr->brake_ns = at_ns;
    tr->brake_by = reason;
    tr->stop_number = reason == ARM_BRAKE_PERSON ? number : 0;
}

/* arm_step to t, with a track point at each grid time on the way. */
static void advance(struct arm_drive *drive, int64_t t)
{
    struct arm_track *tr = &drive->track;

    while (tr->active && tr->next_ns <= t) {
        int64_t g = tr->next_ns;

        arm_step(&drive->arm, g);
        tr->next_ns += ARM_TRACK_STEP_NS;
        track_point(drive, g);
    }
    arm_step(&drive->arm, t);
}

static int target_differs(const struct arm_state *arm, const double target[ARM_JOINTS])
{
    int i;

    for (i = 0; i < ARM_JOINTS; i++) {
        double d = arm->target[i] - target[i];

        if (d > 1e-9 || d < -1e-9) {
            return 1;
        }
    }
    return 0;
}

static void remember(struct arm_drive *drive)
{
    drive->hist_t[drive->hist_next] = drive->arm.t_ns;
    drive->hist_eye[drive->hist_next] = arm_eye_distance(drive->arm.q);
    drive->hist_next = (drive->hist_next + 1) % ARM_HISTORY;
    if (drive->hist_count < ARM_HISTORY) {
        drive->hist_count++;
    }
}

/* Eye distance at t from the history, linear between updates. */
static double eye_at(const struct arm_drive *drive, int64_t t)
{
    unsigned k;
    unsigned oldest;
    int64_t pt = 0;
    double pe = 0.0;
    int have = 0;

    if (drive->hist_count == 0) {
        return arm_eye_distance(drive->arm.q);
    }
    oldest = (drive->hist_next + ARM_HISTORY - drive->hist_count) % ARM_HISTORY;
    for (k = 0; k < drive->hist_count; k++) {
        unsigned i = (oldest + k) % ARM_HISTORY;
        int64_t ht = drive->hist_t[i];
        double he = drive->hist_eye[i];

        if (ht >= t) {
            if (!have || ht == pt) {
                return he;
            }
            return pe + (he - pe) * (double)(t - pt) / (double)(ht - pt);
        }
        pt = ht;
        pe = he;
        have = 1;
    }
    return pe;
}

static void finish(struct arm_drive *drive, struct arm_stop_report *out,
                   int released)
{
    struct arm_stop_report *r = &drive->pending;
    double speed = drive->arm.speed_at_brake < 0.0 ? 0.0
                                                   : drive->arm.speed_at_brake;

    r->speed_m_s = speed;
    r->reaction_m = arm_reaction_path_m(speed, r->brake_ns - r->event_ns);
    r->braking_m = drive->arm.path_m - drive->arm.path_at_brake;
    r->eye_at_stop_m = arm_eye_distance(drive->arm.q);
    r->before_eye = r->eye_at_stop_m > 1e-6;
    r->in_margin = r->eye_at_stop_m >= ARM_STOP_MARGIN_M;
    r->released_early = released;
    r->stopped_ns = released ? 0 : drive->arm.stopped_ns;
    drive->report_due = 0;
    if (out != NULL) {
        *out = *r;
    }
}

int arm_drive_update(struct arm_drive *drive, const struct arm_link_view *view,
                     int64_t now_ns, struct arm_stop_report *out)
{
    struct arm_state *arm = &drive->arm;
    int done = 0;

    if (view->target_seq != 0 && view->target_seq != drive->target_seq_seen) {
        int changed = target_differs(arm, view->target);

        drive->target_seq_seen = view->target_seq;
        arm_set_target(arm, view->target);
        if (changed) {
            track_start(drive);
        }
    }
    if (!arm->brake) {
        if (view->person_in) {
            int64_t at = view->person_since_ns;

            memset(&drive->pending, 0, sizeof drive->pending);
            drive->pending.event_ns = view->event_ns;
            drive->pending.issued_ns = view->stop_issued_ns;
            drive->pending.brake_ns = at < arm->t_ns ? arm->t_ns : at;
            if (view->event_ns > arm->t_ns) {
                advance(drive, view->event_ns);
                remember(drive);
            }
            drive->pending.eye_at_event_m = eye_at(drive, view->event_ns);
            advance(drive, drive->pending.brake_ns);
            remember(drive);
            drive->pending.eye_at_brake_m = arm_eye_distance(arm->q);
            arm_brake(arm, drive->pending.brake_ns);
            track_brake(drive, arm->brake_ns, ARM_BRAKE_PERSON, view->stop_number);
            drive->reason = ARM_BRAKE_PERSON;
            drive->report_due = 1;
            drive->person_stops++;
        } else if (!view->state_ok) {
            arm_brake(arm, view->state_since_ns);
            track_brake(drive, arm->brake_ns, ARM_BRAKE_STATE, 0);
            drive->reason = ARM_BRAKE_STATE;
            drive->state_stops++;
        }
    } else if (!view->person_in && view->state_ok) {
        if (drive->report_due) {
            advance(drive, now_ns);
            finish(drive, out, !arm->stopped);
            done = 1;
        }
        arm_release(arm);
        drive->reason = ARM_BRAKE_NONE;
        if (drive->track.active) {
            /* The brake that counts is the next one. */
            drive->track.brake_ns = 0;
            drive->track.brake_by = ARM_BRAKE_NONE;
            drive->track.stop_number = 0;
        }
    } else if (view->person_in && drive->reason == ARM_BRAKE_STATE) {
        /* Already held by the state; the zone keeps it held. */
        drive->reason = ARM_BRAKE_PERSON;
    }
    advance(drive, now_ns);
    remember(drive);
    if (drive->track.active && arm->brake && arm->stopped && view->person_in) {
        track_end(&drive->track, ARM_TRACK_STOP);
    }
    if (drive->report_due && arm->stopped) {
        finish(drive, out, 0);
        done = 1;
    }
    return done;
}
