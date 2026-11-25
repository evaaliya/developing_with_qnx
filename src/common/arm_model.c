#include "arm_model.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static const double link_m[ARM_JOINTS] = { ARM_LINK1_M, ARM_LINK2_M,
                                           ARM_LINK3_M };

void arm_eye_pose(double q[ARM_JOINTS])
{
    q[0] = ARM_EYE_Q1;
    q[1] = ARM_EYE_Q2;
    q[2] = ARM_EYE_Q3;
}

void arm_home_pose(double q[ARM_JOINTS])
{
    q[0] = ARM_HOME_Q1;
    q[1] = ARM_HOME_Q2;
    q[2] = ARM_HOME_Q3;
}

void arm_tool(const double q[ARM_JOINTS], double *x, double *y)
{
    double phi = 0.0;
    double px = 0.0;
    double py = 0.0;
    int i;

    for (i = 0; i < ARM_JOINTS; i++) {
        phi += q[i];
        px += link_m[i] * cos(phi);
        py += link_m[i] * sin(phi);
    }
    *x = px;
    *y = py;
}

void arm_eye_point(double *x, double *y)
{
    double q[ARM_JOINTS];

    arm_eye_pose(q);
    arm_tool(q, x, y);
}

double arm_eye_distance(const double q[ARM_JOINTS])
{
    double ex;
    double ey;
    double tx;
    double ty;

    arm_eye_point(&ex, &ey);
    arm_tool(q, &tx, &ty);
    return hypot(tx - ex, ty - ey);
}

/* Tool speed per unit joint speed along unit joint direction d. */
static double tool_gain(const double q[ARM_JOINTS], const double d[ARM_JOINTS])
{
    double phi[ARM_JOINTS];
    double vx = 0.0;
    double vy = 0.0;
    double acc = 0.0;
    int i;
    int j;

    for (i = 0; i < ARM_JOINTS; i++) {
        acc += q[i];
        phi[i] = acc;
    }
    for (j = 0; j < ARM_JOINTS; j++) {
        double sx = 0.0;
        double sy = 0.0;

        for (i = j; i < ARM_JOINTS; i++) {
            sx -= link_m[i] * sin(phi[i]);
            sy += link_m[i] * cos(phi[i]);
        }
        vx += sx * d[j];
        vy += sy * d[j];
    }
    return hypot(vx, vy);
}

/*
 * Unit joint direction toward the target and the joint distance left.
 * 0 when the arm is at its target.
 */
static double direction(const struct arm_state *arm, double d[ARM_JOINTS])
{
    double norm = 0.0;
    int i;

    for (i = 0; i < ARM_JOINTS; i++) {
        d[i] = arm->target[i] - arm->q[i];
        norm += d[i] * d[i];
    }
    norm = sqrt(norm);
    if (norm < 1e-12) {
        return 0.0;
    }
    for (i = 0; i < ARM_JOINTS; i++) {
        d[i] /= norm;
    }
    return norm;
}

/* Highest joint speed along d: no joint above its rate limit. */
static double rate_cap(const double d[ARM_JOINTS])
{
    double biggest = 0.0;
    int i;

    for (i = 0; i < ARM_JOINTS; i++) {
        double a = fabs(d[i]);

        if (a > biggest) {
            biggest = a;
        }
    }
    return biggest > 0.0 ? ARM_JOINT_RATE_RAD_S / biggest : 0.0;
}

/* Tool speed the arm would move at now without the brake. */
static double cruise_speed(const struct arm_state *arm)
{
    double d[ARM_JOINTS];
    double gain;
    double w;

    if (direction(arm, d) == 0.0) {
        return 0.0;
    }
    gain = tool_gain(arm->q, d);
    if (gain < 1e-12) {
        return 0.0;
    }
    w = ARM_SPEED_M_S / gain;
    if (w > rate_cap(d)) {
        w = rate_cap(d);
    }
    return w * gain;
}

void arm_init(struct arm_state *arm, const double q[ARM_JOINTS], int64_t now_ns)
{
    memset(arm, 0, sizeof *arm);
    memcpy(arm->q, q, sizeof arm->q);
    memcpy(arm->target, q, sizeof arm->target);
    arm->t_ns = now_ns;
}

void arm_set_target(struct arm_state *arm, const double target[ARM_JOINTS])
{
    memcpy(arm->target, target, sizeof arm->target);
}

void arm_brake(struct arm_state *arm, int64_t at_ns)
{
    if (arm->brake) {
        return;
    }
    arm->brake = 1;
    arm->brake_ns = at_ns < arm->t_ns ? arm->t_ns : at_ns;
    arm->stopped = 0;
    arm->stopped_ns = 0;
    /* Filled in when the model reaches brake_ns. */
    arm->speed_at_brake = -1.0;
    arm->path_at_brake = 0.0;
}

void arm_release(struct arm_state *arm)
{
    arm->brake = 0;
    arm->stopped = 0;
}

/*
 * Moves the joints along the target direction so the tool covers about
 * tool_m, never past the target. Returns the tool path actually covered.
 */
static double move_tool(struct arm_state *arm, double tool_m)
{
    double d[ARM_JOINTS];
    double left;
    double gain;
    double joint;
    double x0;
    double y0;
    double x1;
    double y1;
    int i;

    if (tool_m <= 0.0) {
        return 0.0;
    }
    left = direction(arm, d);
    if (left == 0.0) {
        return 0.0;
    }
    gain = tool_gain(arm->q, d);
    if (gain < 1e-12) {
        return 0.0;
    }
    joint = tool_m / gain;
    arm_tool(arm->q, &x0, &y0);
    if (joint >= left) {
        memcpy(arm->q, arm->target, sizeof arm->q);
    } else {
        for (i = 0; i < ARM_JOINTS; i++) {
            arm->q[i] += d[i] * joint;
        }
    }
    arm_tool(arm->q, &x1, &y1);
    return hypot(x1 - x0, y1 - y0);
}

/* One piece of motion from t_ns to t2 with the brake as it is. */
static void advance(struct arm_state *arm, int64_t t2)
{
    double dt = (double)(t2 - arm->t_ns) * 1e-9;

    if (dt <= 0.0) {
        return;
    }
    if (!arm->brake || arm->t_ns < arm->brake_ns) {
        double v = cruise_speed(arm);
        double moved;

        /* The joint speed follows the tool speed; it is not capped twice. */
        moved = move_tool(arm, v * dt);
        arm->path_m += moved;
        arm->speed = cruise_speed(arm) > 0.0 ? v : 0.0;
        arm->t_ns = t2;
        return;
    }
    if (arm->stopped) {
        arm->speed = 0.0;
        arm->t_ns = t2;
        return;
    }
    {
        double since0 = (double)(arm->t_ns - arm->brake_ns) * 1e-9;
        double v0 = arm->speed_at_brake - ARM_DECEL_M_S2 * since0;
        double v1;
        double run = dt;
        double moved;

        if (v0 < 0.0) {
            v0 = 0.0;
        }
        v1 = v0 - ARM_DECEL_M_S2 * dt;
        if (v1 <= 0.0) {
            run = v0 / ARM_DECEL_M_S2;
            v1 = 0.0;
        }
        moved = move_tool(arm, 0.5 * (v0 + v1) * run);
        arm->path_m += moved;
        arm->speed = v1;
        if (v1 == 0.0 || cruise_speed(arm) == 0.0) {
            arm->speed = 0.0;
            arm->stopped = 1;
            arm->stopped_ns = arm->t_ns + (int64_t)(run * 1e9);
        }
        arm->t_ns = t2;
    }
}

void arm_step(struct arm_state *arm, int64_t now_ns)
{
    while (arm->t_ns < now_ns) {
        int64_t t2 = arm->t_ns + ARM_SUBSTEP_NS;

        if (t2 > now_ns) {
            t2 = now_ns;
        }
        if (arm->brake && arm->speed_at_brake < 0.0 &&
            arm->t_ns < arm->brake_ns && t2 > arm->brake_ns) {
            t2 = arm->brake_ns;
        }
        if (arm->brake && arm->speed_at_brake < 0.0 &&
            arm->t_ns >= arm->brake_ns) {
            arm->speed_at_brake = cruise_speed(arm);
            arm->path_at_brake = arm->path_m;
            if (arm->speed_at_brake == 0.0) {
                arm->stopped = 1;
                arm->stopped_ns = arm->t_ns;
            }
        }
        advance(arm, t2);
    }
    /* A brake that engages exactly at now_ns is noted now. */
    if (arm->brake && arm->speed_at_brake < 0.0 && arm->t_ns >= arm->brake_ns) {
        arm->speed_at_brake = cruise_speed(arm);
        arm->path_at_brake = arm->path_m;
        if (arm->speed_at_brake == 0.0) {
            arm->stopped = 1;
            arm->stopped_ns = arm->t_ns;
        }
    }
}

double arm_reaction_path_m(double speed_m_s, int64_t delay_ns)
{
    if (speed_m_s <= 0.0 || delay_ns <= 0) {
        return 0.0;
    }
    return speed_m_s * (double)delay_ns * 1e-9;
}

double arm_braking_path_m(double speed_m_s)
{
    if (speed_m_s <= 0.0) {
        return 0.0;
    }
    return speed_m_s * speed_m_s / (2.0 * ARM_DECEL_M_S2);
}
