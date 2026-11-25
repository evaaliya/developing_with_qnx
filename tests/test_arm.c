#include "arm_drive.h"
#include "arm_model.h"
#include "arm_plan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_near(const char *name, double got, double want, double tol)
{
    if (fabs(got - want) <= tol) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %.6f, want %.6f +- %.6f)\n", name, got, want, tol);
    failures++;
}

static void expect_int(const char *name, long long got, long long want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %lld, want %lld)\n", name, got, want);
    failures++;
}

#define MS 1000000LL

static struct signal urgent(int person, int64_t issued, int64_t event)
{
    struct signal s;

    memset(&s, 0, sizeof s);
    s.class_id = SIGNAL_CLASS_URGENT;
    s.issued_ns = issued;
    s.arm.flags = person ? SIGNAL_ARM_PERSON_IN : 0;
    s.arm.event_ns = event;
    return s;
}

static struct signal command_to(int eye, int64_t issued)
{
    struct signal s;

    memset(&s, 0, sizeof s);
    s.class_id = SIGNAL_CLASS_COMMAND;
    s.issued_ns = issued;
    s.arm.flags = SIGNAL_ARM_TARGET;
    s.arm.joint_mrad[0] = eye ? 300 : -100;
    s.arm.joint_mrad[1] = eye ? 1200 : 1600;
    s.arm.joint_mrad[2] = eye ? 500 : 1100;
    return s;
}

/* Runs the drive in 2 ms steps from t0 to t1; the last report in *rep. */
static int run(struct arm_drive *d, struct arm_link *link, int64_t t0,
               int64_t t1, struct arm_stop_report *rep)
{
    int64_t t;
    int reports = 0;

    for (t = t0; t <= t1; t += 2 * MS) {
        struct arm_link_view v;
        struct arm_stop_report r;

        arm_link_read(link, &v);
        if (arm_drive_update(d, &v, t, &r)) {
            *rep = r;
            reports++;
        }
    }
    return reports;
}

static void test_model(void)
{
    double q[ARM_JOINTS];
    double e[ARM_JOINTS];
    double x;
    double y;
    struct arm_state a;

    arm_eye_point(&x, &y);
    expect_near("eye x", x, 0.3653, 0.0001);
    expect_near("eye y", y, 0.5583, 0.0001);
    arm_eye_pose(e);
    expect_near("eye pose is at the eye", arm_eye_distance(e), 0.0, 1e-12);
    arm_home_pose(q);
    expect_near("home is 199.5 mm from the eye", arm_eye_distance(q), 0.1995,
                0.0002);

    arm_init(&a, q, 0);
    arm_set_target(&a, e);
    arm_step(&a, 400 * MS);
    expect_near("approach at 250 mm/s: 100 mm in 400 ms", a.path_m, 0.100,
                0.0005);
    expect_near("speed 250 mm/s", a.speed, ARM_SPEED_M_S, 1e-9);
    expect_near("eye distance falls with the path", arm_eye_distance(a.q),
                0.0995, 0.002);
    arm_step(&a, 1000 * MS);
    expect_near("target reached is the eye", arm_eye_distance(a.q), 0.0, 1e-9);
    expect_near("at rest at the target", a.speed, 0.0, 1e-12);
    arm_step(&a, 900 * MS);
    expect_int("an earlier time does nothing", a.t_ns, 1000 * MS);

    arm_init(&a, q, 0);
    arm_set_target(&a, e);
    arm_step(&a, 200 * MS);
    arm_brake(&a, 201 * MS);
    arm_step(&a, 202 * MS);
    expect_near("speed at brake", a.speed_at_brake, ARM_SPEED_M_S, 1e-9);
    expect_near("path at brake: 50.25 mm", a.path_at_brake, 0.05025, 0.0002);
    arm_step(&a, 600 * MS);
    expect_int("stopped", a.stopped, 1);
    expect_near("braking path v^2/2a = 25 mm", a.path_m - a.path_at_brake,
                0.025, 0.0001);
    expect_near("stop 200 ms after the brake", (double)(a.stopped_ns - 201 * MS),
                200e6, 1e6);
    arm_brake(&a, 700 * MS);
    expect_int("a held brake keeps its time", a.brake_ns, 201 * MS);
    arm_set_target(&a, q);
    arm_step(&a, 800 * MS);
    expect_near("a held brake does not move", a.path_m - a.path_at_brake, 0.025,
                0.0001);
    arm_release(&a);
    arm_step(&a, 900 * MS);
    expect_near("released, it moves to the new target", a.speed, ARM_SPEED_M_S,
                1e-9);

    arm_init(&a, q, 0);
    arm_brake(&a, 10 * MS);
    arm_step(&a, 20 * MS);
    expect_int("brake at rest: stopped at once", a.stopped, 1);
    expect_near("brake at rest: no braking path", a.path_m, 0.0, 1e-12);

    arm_init(&a, q, 50 * MS);
    arm_brake(&a, 10 * MS);
    expect_int("a brake time before the model counts from the model",
               a.brake_ns, 50 * MS);

    expect_near("reaction path 250 mm/s x 1 ms", arm_reaction_path_m(0.25, MS),
                0.00025, 1e-12);
    expect_near("reaction path 250 mm/s x 31 ms",
                arm_reaction_path_m(0.25, 31 * MS), 0.00775, 1e-12);
    expect_near("no reaction path at rest", arm_reaction_path_m(0.0, MS), 0.0,
                0.0);
    expect_near("no reaction path for a negative delay",
                arm_reaction_path_m(0.25, -MS), 0.0, 0.0);
    expect_near("braking path at 250 mm/s", arm_braking_path_m(0.25), 0.025,
                1e-12);
    expect_near("braking path at rest", arm_braking_path_m(0.0), 0.0, 0.0);
}

static void test_drive(void)
{
    struct arm_link link;
    struct arm_drive d;
    struct arm_stop_report rep;
    struct signal s;
    int n;

    arm_link_init(&link);
    arm_drive_init(&d, 0);
    memset(&rep, 0, sizeof rep);
    s = command_to(1, 1 * MS);
    arm_link_on_command(&link, &s);
    n = run(&d, &link, 0, 100 * MS, &rep);
    expect_int("held until the state lets it move: no report", n, 0);
    expect_near("held: no path", d.arm.path_m, 0.0, 0.0);

    arm_link_on_state(&link, 1, 100 * MS);
    n = run(&d, &link, 100 * MS, 400 * MS, &rep);
    expect_near("state ok: moving to the eye", d.arm.speed, ARM_SPEED_M_S, 1e-9);

    s = urgent(1, 401 * MS, 400 * MS);
    expect_int("person in turns the zone on",
               arm_link_on_urgent(&link, &s, 402 * MS), 1);
    expect_int("the same again does not",
               arm_link_on_urgent(&link, &s, 403 * MS), 0);
    n = run(&d, &link, 402 * MS, 900 * MS, &rep);
    expect_int("one stop report", n, 1);
    expect_int("delay event to brake 2 ms", rep.brake_ns - rep.event_ns, 2 * MS);
    expect_int("issued kept", rep.issued_ns, 401 * MS);
    expect_near("speed at the brake", rep.speed_m_s, ARM_SPEED_M_S, 1e-9);
    expect_near("reaction path 0.5 mm", rep.reaction_m, 0.0005, 1e-9);
    expect_near("braking path 25 mm", rep.braking_m, 0.025, 0.0001);
    expect_near("eye distance falls by the reaction path",
                rep.eye_at_event_m - rep.eye_at_brake_m, 0.0005, 0.00005);
    expect_near("eye distance at the brake", rep.eye_at_brake_m, 0.1995 - 0.0755,
                0.002);
    expect_near("rests braking path short of the brake point",
                rep.eye_at_brake_m - rep.eye_at_stop_m, 0.025, 0.0006);
    expect_int("before the eye", rep.before_eye, 1);
    expect_int("in the margin", rep.in_margin, 1);
    expect_int("not released early", rep.released_early, 0);
    expect_int("rest 200 ms after the brake", (rep.stopped_ns - rep.brake_ns +
                                               MS / 2) / MS, 200);

    s = urgent(0, 300 * MS, 0);
    expect_int("an older URGENT does not clear the zone",
               arm_link_on_urgent(&link, &s, 901 * MS), 0);
    expect_int("zone still on", link.person_in, 1);
    s = command_to(0, 905 * MS);
    arm_link_on_command(&link, &s);
    n = run(&d, &link, 902 * MS, 1000 * MS, &rep);
    expect_int("held by the zone: no move", d.arm.brake, 1);
    s = urgent(0, 1001 * MS, 1000 * MS);
    expect_int("person out is not a brake",
               arm_link_on_urgent(&link, &s, 1002 * MS), 0);
    n = run(&d, &link, 1002 * MS, 1100 * MS, &rep);
    expect_int("released: no new report", n, 0);
    expect_near("released: going home", d.arm.speed, ARM_SPEED_M_S, 1e-9);
    s = command_to(1, 904 * MS);
    arm_link_on_command(&link, &s);
    {
        struct arm_link_view v;

        arm_link_read(&link, &v);
        expect_near("an older COMMAND does not move the target", v.target[1],
                    1.6, 1e-9);
    }

    /* SAFE: the state brakes the arm, no stop report. */
    arm_link_on_state(&link, 0, 1101 * MS);
    n = run(&d, &link, 1102 * MS, 1500 * MS, &rep);
    expect_int("SAFE brake: no person report", n, 0);
    expect_int("SAFE brake counted", d.state_stops, 1);
    expect_int("SAFE: stopped", d.arm.stopped, 1);
    expect_int("SAFE brake time is the state time", d.arm.brake_ns, 1101 * MS);
}

static void test_late_and_early(void)
{
    struct arm_link link;
    struct arm_drive d;
    struct arm_stop_report rep;
    struct signal s;
    int n;

    /* The zone signal comes too late: the tool reaches the eye. */
    arm_link_init(&link);
    arm_drive_init(&d, 0);
    arm_link_on_state(&link, 1, 0);
    s = command_to(1, 0);
    arm_link_on_command(&link, &s);
    run(&d, &link, 0, 780 * MS, &rep);
    s = urgent(1, 781 * MS, 780 * MS);
    arm_link_on_urgent(&link, &s, 782 * MS);
    memset(&rep, 0, sizeof rep);
    n = run(&d, &link, 782 * MS, 1200 * MS, &rep);
    expect_int("late zone: a report", n, 1);
    expect_int("late zone: the tool reaches the eye", rep.before_eye, 0);
    expect_int("late zone: not in the margin", rep.in_margin, 0);

    /* Person out before the arm rests: the report says so. */
    arm_link_init(&link);
    arm_drive_init(&d, 0);
    arm_link_on_state(&link, 1, 0);
    s = command_to(1, 0);
    arm_link_on_command(&link, &s);
    run(&d, &link, 0, 200 * MS, &rep);
    s = urgent(1, 201 * MS, 200 * MS);
    arm_link_on_urgent(&link, &s, 202 * MS);
    run(&d, &link, 202 * MS, 250 * MS, &rep);
    s = urgent(0, 251 * MS, 250 * MS);
    arm_link_on_urgent(&link, &s, 252 * MS);
    memset(&rep, 0, sizeof rep);
    n = run(&d, &link, 252 * MS, 300 * MS, &rep);
    expect_int("early release: a report", n, 1);
    expect_int("early release: said", rep.released_early, 1);
    expect_int("early release: no rest time", rep.stopped_ns, 0);

    /* A target being written is not read half-way. */
    link.target_seq = 7;
    {
        struct arm_link_view v;

        arm_link_read(&link, &v);
        expect_int("odd sequence: no target this step", v.target_seq, 0);
    }
    {
        struct signal b;

        memset(&b, 0, sizeof b);
        b.class_id = SIGNAL_CLASS_BACKGROUND;
        expect_int("BACKGROUND is not a zone signal",
                   arm_link_on_urgent(&link, &b, 0), 0);
        expect_int("null link", arm_link_on_urgent(NULL, &b, 0), 0);
    }
}

/*
 * The track for the replay (D-085): the tool's eye distance on a 2 ms
 * grid of model time from the start of the approach to the stop or the
 * touch, even when a model step is late.
 */
static void step_to(struct arm_drive *d, struct arm_link *link, int64_t t)
{
    struct arm_link_view v;
    struct arm_stop_report r;

    arm_link_read(link, &v);
    (void)arm_drive_update(d, &v, t, &r);
}

static void test_track(void)
{
    struct arm_link link;
    struct arm_drive d;
    struct signal s;
    int64_t t;
    unsigned i;
    int steady = 1;
    int grid = 1;
    double prev;

    /* A stop: person in at 300 ms, brake at 302 ms by URGENT 4242. */
    arm_link_init(&link);
    arm_drive_init(&d, 0);
    arm_link_on_state(&link, 1, 0);
    s = command_to(1, 0);
    arm_link_on_command(&link, &s);
    expect_int("no track before the first target", d.track.count, 0);
    for (t = 0; t <= 290 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
        if (t == 100 * MS) {
            /* The schedule repeats the same target: no new track. */
            s = command_to(1, 100 * MS);
            arm_link_on_command(&link, &s);
        }
    }
    /* One late model step: 9 ms. */
    step_to(&d, &link, 299 * MS);
    s = urgent(1, 301 * MS, 300 * MS);
    s.number = 4242;
    arm_link_on_urgent(&link, &s, 302 * MS);
    for (t = 301 * MS; t <= 900 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
    }
    expect_int("track starts at the approach", d.track.start_ns, 0);
    expect_int("track ends at the stop", d.track.end, ARM_TRACK_STOP);
    expect_int("brake by the person", d.track.brake_by, ARM_BRAKE_PERSON);
    expect_int("brake time from the receive thread", d.track.brake_ns, 302 * MS);
    expect_int("the URGENT that braked", (long long)d.track.stop_number, 4242);
    expect_int("no touch", d.track.touch_ns, 0);
    {
        int64_t last_t = d.track.start_ns + (int64_t)(d.track.count - 1) * 2 * MS;
        int64_t off = last_t - d.arm.stopped_ns;

        expect_int("samples to the rest, one per 2 ms",
                   off >= -2 * MS && off <= 2 * MS, 1);
    }
    prev = d.track.eye_um[0];
    for (i = 1; i < d.track.count; i++) {
        double e = d.track.eye_um[i];

        if (e > prev + 1.0) {
            steady = 0;
        }
        if (prev - e > 510.0) {
            grid = 0;
        }
        prev = e;
    }
    expect_int("the tool never moves away from the eye on the way in", steady, 1);
    expect_int("no gap of more than 0.5 mm, also over the late step", grid, 1);
    expect_near("first sample at home, 199.5 mm", d.track.eye_um[0] / 1000.0, 199.5, 0.2);
    expect_near("sample at the event, 75 mm on", d.track.eye_um[150] / 1000.0,
                199.5 - 75.0, 2.0);
    expect_near("last sample is the rest", d.track.eye_um[d.track.count - 1] / 1e6,
                arm_eye_distance(d.arm.q), 1e-6);
    s = command_to(0, 905 * MS);
    arm_link_on_command(&link, &s);
    s = urgent(0, 906 * MS, 906 * MS);
    arm_link_on_urgent(&link, &s, 907 * MS);
    for (t = 902 * MS; t <= 1200 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
    }
    expect_int("a finished track is kept when the arm goes home", d.track.start_ns, 0);
    expect_int("and its end stays", d.track.end, ARM_TRACK_STOP);

    /* No URGENT gets through: the tool goes on and touches the eye. */
    arm_link_init(&link);
    arm_drive_init(&d, 0);
    arm_link_on_state(&link, 1, 0);
    for (t = 0; t <= 8 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
    }
    s = command_to(1, 10 * MS);
    arm_link_on_command(&link, &s);
    for (t = 10 * MS; t <= 1300 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
    }
    expect_int("touch ends the track", d.track.end, ARM_TRACK_TOUCH);
    expect_int("touch: the approach starts at the step with the target",
               d.track.start_ns, 8 * MS);
    expect_near("touch about 799 ms into the approach",
                (d.track.touch_ns - d.track.start_ns) / 1e6, 799.0, 2.0);
    expect_int("touch: last sample at the eye", d.track.eye_um[d.track.count - 1], 0);
    expect_int("touch: no brake", d.track.brake_by, ARM_BRAKE_NONE);

    /* SAFE holds the arm when the person comes: the state braked it. */
    arm_link_init(&link);
    arm_drive_init(&d, 0);
    arm_link_on_state(&link, 1, 0);
    s = command_to(1, 0);
    arm_link_on_command(&link, &s);
    for (t = 0; t <= 200 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
    }
    arm_link_on_state(&link, 0, 201 * MS);
    for (t = 202 * MS; t <= 300 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
    }
    expect_int("SAFE alone does not end the track", d.track.end, ARM_TRACK_OPEN);
    s = urgent(1, 301 * MS, 300 * MS);
    s.number = 77;
    arm_link_on_urgent(&link, &s, 302 * MS);
    for (t = 302 * MS; t <= 500 * MS; t += 2 * MS) {
        step_to(&d, &link, t);
    }
    expect_int("person in while held ends the track", d.track.end, ARM_TRACK_STOP);
    expect_int("brake by the state", d.track.brake_by, ARM_BRAKE_STATE);
    expect_int("brake time is the state change", d.track.brake_ns, 201 * MS);
    expect_int("no stop number for a state brake", (long long)d.track.stop_number, 0);
}

/* Eye distance at t from the track, linear between grid points, m. */
static double track_eye_at(const struct arm_track *tr, int64_t t)
{
    int64_t off = t - tr->start_ns;
    unsigned i = (unsigned)(off / ARM_TRACK_STEP_NS);
    double f = (double)(off % ARM_TRACK_STEP_NS) / (double)ARM_TRACK_STEP_NS;

    if (i + 1 >= tr->count) {
        return tr->eye_um[tr->count - 1] / 1e6;
    }
    return (tr->eye_um[i] + (tr->eye_um[i + 1] - tr->eye_um[i]) * f) / 1e6;
}

/*
 * One planned approach (D-089, D-090): the target arrives between model
 * steps, steps come late by up to 1.9 ms, "person in" is issued at the
 * approach start plus the planned time, and the brake follows delay_ms
 * later. *event_m: eye distance at the event by the track; *rest_m: where
 * the tool rests. The track end is returned.
 */
static int planned_stop(int64_t delay_ns, int64_t offset_ns, double *event_m,
                        double *rest_m)
{
    struct arm_link link;
    struct arm_drive d;
    struct signal s;
    int64_t t = 1000 * MS;
    int64_t event = 0;
    int64_t brake = 0;
    int sent = 0;
    unsigned k = 0;

    arm_link_init(&link);
    arm_drive_init(&d, t);
    arm_link_on_state(&link, 1, t);
    s = command_to(1, t);
    while (t < 3000 * MS) {
        /* Steps 2 ms apart, every third one late by up to 1.9 ms. */
        int64_t late = (k % 3 == 2) ? (int64_t)((k * 7) % 20) * 95000LL : 0;

        if (!sent && t >= 1007 * MS) {
            arm_link_on_command(&link, &s);
            sent = 1;
        }
        if (d.track.count > 0 && event == 0) {
            event = d.track.start_ns + offset_ns;
            brake = event + delay_ns;
        }
        if (brake != 0 && t + 2 * MS + late > brake && !link.person_in) {
            struct signal u = urgent(1, event, event);

            u.number = 9;
            arm_link_on_urgent(&link, &u, brake);
        }
        t += 2 * MS;
        step_to(&d, &link, t + late);
        k++;
        if (d.track.end != ARM_TRACK_OPEN) {
            break;
        }
    }
    *event_m = track_eye_at(&d.track, event);
    *rest_m = d.track.end == ARM_TRACK_TOUCH ? 0.0 : arm_eye_distance(d.arm.q);
    return d.track.end;
}

static void test_plan(void)
{
    int64_t offset = arm_plan_time_to_distance_ns(arm_plan_event_distance_m());
    static const int delays_ms[] = { 0, 10, 31 };
    double event_m;
    double rest_m;
    unsigned i;
    int end;
    struct arm_approach a;
    int64_t start = 0;
    int32_t eye_um = 0;

    expect_near("plan margin is 31 ms x 250 mm/s", ARM_PLAN_MARGIN_M, 0.00775, 1e-12);
    expect_near("event distance is margin + braking path", arm_plan_event_distance_m(),
                0.03275, 1e-12);
    expect_near("time to the event point", (double)offset / 1e6, 668.141, 0.001);
    for (i = 0; i < sizeof delays_ms / sizeof delays_ms[0]; i++) {
        char name[96];
        double want = 0.007754 - ARM_SPEED_M_S * delays_ms[i] * 1e-3;

        end = planned_stop(delays_ms[i] * MS, offset, &event_m, &rest_m);
        snprintf(name, sizeof name, "delay %d ms: tool at 32.75 mm at the event",
                 delays_ms[i]);
        expect_near(name, event_m, 0.03275, 0.00001);
        snprintf(name, sizeof name, "delay %d ms: rests 0.25 mm/ms past the plan",
                 delays_ms[i]);
        expect_near(name, rest_m, want, 0.00002);
        snprintf(name, sizeof name, "delay %d ms: a stop, not a touch", delays_ms[i]);
        expect_int(name, end, ARM_TRACK_STOP);
    }
    end = planned_stop(40 * MS, offset, &event_m, &rest_m);
    expect_int("delay 40 ms (over the deadline): the tool touches the eye", end,
               ARM_TRACK_TOUCH);
    expect_near("delay 40 ms: the event point is the same", event_m, 0.03275, 0.00001);

    memset(&a, 0, sizeof a);
    expect_int("no approach yet", arm_approach_read(&a, &start, &eye_um), 0);
    arm_approach_publish(&a, 123456789, 199500);
    expect_int("approach published", arm_approach_read(&a, &start, &eye_um), 1);
    expect_int("approach start", start, 123456789);
    expect_int("approach start distance", eye_um, 199500);
    a.magic = 7;
    expect_int("a foreign object is refused", arm_approach_read(&a, &start, &eye_um), -1);
}

int main(void)
{
    test_model();
    test_drive();
    test_late_and_early();
    test_track();
    test_plan();
    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
