#ifndef ARM_DRIVE_H
#define ARM_DRIVE_H

/*
 * The arm in the sink (D-080). Two threads share struct arm_link:
 * the receive thread writes it, the model thread reads it. No locks:
 * the receive thread never waits for the model, so the arm cannot delay
 * an URGENT. The zone flag is one atomic word, written last; a joint
 * target is guarded by a sequence count (the reader retries).
 * No QNX headers and no clock: the caller passes the time.
 */

#include "arm_model.h"
#include "signal_msg.h"

#include <stdint.h>

struct arm_link {
    /* Zone, written by the receive thread only. */
    int person_in;
    int64_t person_since_ns;
    int64_t event_ns;
    int64_t stop_issued_ns;
    /* Number of the URGENT that turned the zone on (D-085). */
    uint64_t stop_number;
    /* Issue time of the newest URGENT that set the zone. */
    int64_t zone_issued_ns;
    /* Network state lets the arm move (NORMAL, DEGRADED, RECOVERING). */
    int state_ok;
    int64_t state_since_ns;
    /* Joint target, odd seq while it is being written. */
    uint32_t target_seq;
    int64_t target_issued_ns;
    int32_t target_mrad[ARM_JOINTS];
};

struct arm_link_view {
    int person_in;
    int64_t person_since_ns;
    int64_t event_ns;
    int64_t stop_issued_ns;
    uint64_t stop_number;
    int state_ok;
    int64_t state_since_ns;
    uint32_t target_seq;
    double target[ARM_JOINTS];
};

/* Nobody in the zone, state not yet ok (the arm is held), no target. */
void arm_link_init(struct arm_link *link);

/*
 * An URGENT arrived at now_ns. A zone flag from an URGENT issued before
 * the one that last set the zone is ignored (two source threads may
 * race). 1 when this URGENT turned the zone on (the brake time is
 * now_ns), 0 otherwise.
 */
int arm_link_on_urgent(struct arm_link *link, const struct signal *sig,
                       int64_t now_ns);

/* A COMMAND with a joint target. An older target than the last is ignored. */
void arm_link_on_command(struct arm_link *link, const struct signal *sig);

/* The network state changed at now_ns; ok when it lets the arm move. */
void arm_link_on_state(struct arm_link *link, int ok, int64_t now_ns);

void arm_link_read(const struct arm_link *link, struct arm_link_view *view);

/* One stop after "person in", printed by the sink once the arm rests. */
struct arm_stop_report {
    int64_t event_ns;
    int64_t issued_ns;
    int64_t brake_ns;
    int64_t stopped_ns;
    double speed_m_s;
    double reaction_m;
    double braking_m;
    double eye_at_event_m;
    double eye_at_brake_m;
    double eye_at_stop_m;
    /* 1 when the tool rests short of the eye. */
    int before_eye;
    /* 1 when it rests at least ARM_STOP_MARGIN_M from the eye. */
    int in_margin;
    /* 1 when the brake let go before the arm came to rest. */
    int released_early;
};

#define ARM_HISTORY 256

enum arm_brake_reason {
    ARM_BRAKE_NONE = 0,
    ARM_BRAKE_PERSON = 1,
    ARM_BRAKE_STATE = 2
};

/*
 * The track for the replay (D-085): the tool's eye distance on a fixed
 * grid of model time from the start of an approach (the first step with
 * a joint target unlike the last) to its end. Points are computed on the
 * grid even when a model step comes late. The model thread writes it;
 * count and end are published with release stores, so another thread
 * may read the points below count at any time.
 */
#define ARM_TRACK_STEP_NS 2000000LL
#define ARM_TRACK_CAP 1024
/* At or under this distance the tool touches the eye (as before_eye). */
#define ARM_TOUCH_M 1e-6

enum arm_track_end {
    ARM_TRACK_OPEN = 0,
    /* The arm rests after a brake while the person is in the zone. */
    ARM_TRACK_STOP = 1,
    /* The tool reached the eye. */
    ARM_TRACK_TOUCH = 2,
    /* ARM_TRACK_CAP points and neither. */
    ARM_TRACK_FULL = 3
};

struct arm_track {
    int active;
    int64_t start_ns;
    int64_t next_ns;
    double last_speed;
    unsigned count;
    int end;
    /* The brake during this track: when, and why (enum arm_brake_reason). */
    int64_t brake_ns;
    int brake_by;
    /* The URGENT that braked, when the person braked it; else 0. */
    uint64_t stop_number;
    /* When the tool reached the eye, to the grid step; else 0. */
    int64_t touch_ns;
    int32_t eye_um[ARM_TRACK_CAP];
};

const char *arm_track_end_name(int end);
const char *arm_brake_reason_name(int reason);

struct arm_drive {
    struct arm_state arm;
    struct arm_track track;
    uint32_t target_seq_seen;
    enum arm_brake_reason reason;
    int report_due;
    struct arm_stop_report pending;
    unsigned person_stops;
    unsigned state_stops;
    /* Model time, tool path and eye distance at each update. */
    int64_t hist_t[ARM_HISTORY];
    double hist_eye[ARM_HISTORY];
    unsigned hist_next;
    unsigned hist_count;
};

/* At rest at home, braked until the state lets it move. */
void arm_drive_init(struct arm_drive *drive, int64_t now_ns);

/*
 * One model step: take the link, brake or release, advance to now_ns.
 * 1 when a stop after "person in" is complete; *out holds it.
 */
int arm_drive_update(struct arm_drive *drive, const struct arm_link_view *view,
                     int64_t now_ns, struct arm_stop_report *out);

#endif
