/*
 * Joint angles for the replay video (B31), from the same arm model the
 * sink runs (src/common/arm_model.c, D-079). The replay files hold only
 * the eye distance on a 2 ms grid (D-085), so the 3D view needs the
 * angles back: this program runs the model again from the home pose
 * toward the eye pose, brakes it at the journal's brake time, and prints
 * each grid point. scripts/replay_rrd.py compares the eye distance here
 * with the journal in every frame (0.01 mm) and fails otherwise.
 *
 * The call order follows arm_drive.c: the approach starts with the arm at
 * rest at home, the target set to the eye pose and a point at time 0;
 * grid points come from arm_step to each grid time; the brake is set by
 * stepping to the brake time first and then arm_brake at that time.
 *
 * Usage: arm-replay-pose STEP_NS SAMPLES BRAKE_NS
 *   times in ns from the start of the approach; BRAKE_NS -1 = no brake.
 * Output, one line per grid point:
 *   P index t_ns eye_um q1 q2 q3 x1 y1 x2 y2 x3 y3   (metres, radians)
 * where (x1,y1) is the elbow, (x2,y2) the wrist and (x3,y3) the tool tip;
 * then one line: E eye_x eye_y stopped stopped_ns
 */

#include "arm_model.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_i64(const char *s, int64_t *out)
{
    char *end = NULL;
    long long v;

    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

static void joints(const double q[ARM_JOINTS], double xy[ARM_JOINTS][2])
{
    static const double len[ARM_JOINTS] = { ARM_LINK1_M, ARM_LINK2_M,
                                            ARM_LINK3_M };
    double phi = 0.0;
    double x = 0.0;
    double y = 0.0;
    int i;

    for (i = 0; i < ARM_JOINTS; i++) {
        phi += q[i];
        x += len[i] * cos(phi);
        y += len[i] * sin(phi);
        xy[i][0] = x;
        xy[i][1] = y;
    }
}

static void print_point(unsigned i, int64_t t, const struct arm_state *arm)
{
    double xy[ARM_JOINTS][2];
    double um = arm_eye_distance(arm->q) * 1e6;
    double tx;
    double ty;

    joints(arm->q, xy);
    /* The tip of the chain must be the model's own tool point. */
    arm_tool(arm->q, &tx, &ty);
    if (fabs(tx - xy[2][0]) > 1e-12 || fabs(ty - xy[2][1]) > 1e-12) {
        fprintf(stderr, "arm-replay-pose: chain tip differs from arm_tool\n");
        exit(1);
    }
    printf("P %u %" PRId64 " %" PRId32 " %.17g %.17g %.17g "
           "%.17g %.17g %.17g %.17g %.17g %.17g\n",
           i, t, (int32_t)(um + 0.5), arm->q[0], arm->q[1], arm->q[2],
           xy[0][0], xy[0][1], xy[1][0], xy[1][1], xy[2][0], xy[2][1]);
}

int main(int argc, char **argv)
{
    struct arm_state arm;
    double home[ARM_JOINTS];
    double eye[ARM_JOINTS];
    double ex;
    double ey;
    int64_t step;
    int64_t samples;
    int64_t brake;
    int braked = 0;
    int64_t i;

    if (argc != 4 || parse_i64(argv[1], &step) != 0 ||
        parse_i64(argv[2], &samples) != 0 || parse_i64(argv[3], &brake) != 0 ||
        step <= 0 || samples <= 0 || samples > 100000 || brake < -1) {
        fprintf(stderr, "usage: arm-replay-pose STEP_NS SAMPLES BRAKE_NS "
                        "(BRAKE_NS -1 = no brake)\n");
        return 2;
    }

    arm_home_pose(home);
    arm_eye_pose(eye);
    arm_init(&arm, home, 0);
    arm_set_target(&arm, eye);
    print_point(0, 0, &arm);

    for (i = 1; i < samples; i++) {
        int64_t g = i * step;

        if (!braked && brake >= 0 && brake <= g) {
            arm_step(&arm, brake);
            arm_brake(&arm, brake);
            braked = 1;
        }
        arm_step(&arm, g);
        print_point((unsigned)i, g, &arm);
    }

    arm_eye_point(&ex, &ey);
    printf("E %.17g %.17g %d %" PRId64 "\n", ex, ey, arm.stopped,
           arm.stopped ? arm.stopped_ns : (int64_t)-1);
    return 0;
}
