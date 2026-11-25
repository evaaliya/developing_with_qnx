#ifndef ARM_MODEL_H
#define ARM_MODEL_H

/*
 * A planar arm with three revolute joints, driven by joint targets and
 * stopped by a brake. No QNX headers and no clock: the caller passes the
 * time. The parameters were fixed before any run (D-079) and are not
 * tuned to a result.
 *
 * Motion: from the present joint angles straight toward the target in
 * joint space. The joint speed is scaled so the tool tip moves at the
 * approach speed, unless a joint would pass its rate limit. Speed
 * changes at once when moving; only the brake ramps (a simplification).
 * Brake: from the instant it engages, the tool speed falls at the
 * braking deceleration along the same path until the arm rests.
 */

#include <stdint.h>

#define ARM_JOINTS 3

/* D-079. Lengths in metres, angles in radians. */
#define ARM_LINK1_M 0.40
#define ARM_LINK2_M 0.35
#define ARM_LINK3_M 0.10
/* Tool speed while moving near a person: 250 mm/s (D-079). */
#define ARM_SPEED_M_S 0.25
/* Tool deceleration once the brake engages: 1.25 m/s^2, 200 ms from full speed. */
#define ARM_DECEL_M_S2 1.25
/* No joint turns faster than this. */
#define ARM_JOINT_RATE_RAD_S 2.0
/* The tool must rest at least this far from the eye. */
#define ARM_STOP_MARGIN_M 0.010
/* Integration step inside arm_step; the caller's period may be longer. */
#define ARM_SUBSTEP_NS 500000LL

/* The pose whose tool tip is the eye, and the home pose (D-079). */
#define ARM_EYE_Q1 0.30
#define ARM_EYE_Q2 1.20
#define ARM_EYE_Q3 0.50
#define ARM_HOME_Q1 -0.10
#define ARM_HOME_Q2 1.60
#define ARM_HOME_Q3 1.10

struct arm_state {
    double q[ARM_JOINTS];
    double target[ARM_JOINTS];
    int64_t t_ns;
    /* Tool speed along the path now, m/s. */
    double speed;
    /* Total tool path, m. */
    double path_m;
    int brake;
    int64_t brake_ns;
    double speed_at_brake;
    double path_at_brake;
    /* 1 once the arm rests after the brake engaged. */
    int stopped;
    int64_t stopped_ns;
};

void arm_eye_pose(double q[ARM_JOINTS]);
void arm_home_pose(double q[ARM_JOINTS]);

/* Tool tip of pose q, metres. */
void arm_tool(const double q[ARM_JOINTS], double *x, double *y);

/* Where the eye is: the tool tip of the eye pose. */
void arm_eye_point(double *x, double *y);

/* Distance from the tool tip of pose q to the eye, m. */
double arm_eye_distance(const double q[ARM_JOINTS]);

/* At rest in pose q at time now_ns, target q, brake off. */
void arm_init(struct arm_state *arm, const double q[ARM_JOINTS], int64_t now_ns);

/* A new joint target. A held brake keeps the arm where it is. */
void arm_set_target(struct arm_state *arm, const double target[ARM_JOINTS]);

/*
 * The brake engages at at_ns (not before the model time; an earlier
 * time counts from the model time). A brake already on stays as it was.
 */
void arm_brake(struct arm_state *arm, int64_t at_ns);

/* The brake lets go; the arm moves on toward its target. */
void arm_release(struct arm_state *arm);

/* Advance the model to now_ns. An earlier time does nothing. */
void arm_step(struct arm_state *arm, int64_t now_ns);

/* Path the tool covers while the signal travels: speed x delay, m. */
double arm_reaction_path_m(double speed_m_s, int64_t delay_ns);

/* Path to stop from speed_m_s at ARM_DECEL_M_S2: v^2 / 2a, m. */
double arm_braking_path_m(double speed_m_s);

#endif
