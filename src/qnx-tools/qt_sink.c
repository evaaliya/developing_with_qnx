/*
 * sink of the "QNX tools" network (D-098 (5), (7)).
 *
 * One receive thread on the channel from name_attach; it inherits the
 * sender's priority. The arm is the same model as in Nerve (arm_model.c,
 * arm_drive.c, arm_plan.c unchanged): an URGENT with "person in" brakes
 * it from the receive thread with atomic stores before the reply (D-080).
 * The watchdog is TimerTimeout on MsgReceive with the same window as the
 * Nerve sink (its urgent-gap-ms argument, 1000 ms by default): no URGENT
 * in the window puts the sink in SAFE (the arm brakes), the first URGENT
 * after that puts it in NORMAL. There is no DEGRADED, RECOVERING, run of
 * three URGENT, LOCKED_SAFE, drop notice or ledger: nothing is accounted
 * in the network, the host counts issued against received from the logs.
 * The approach start goes to shared memory for the zone sensor (D-090).
 *
 *   sink [-o FILE] [-q] [-c 0] [-r] <signal-count> [urgent-gap-ms]
 */
#define _QNX_SOURCE

#include "arm_drive.h"
#include "arm_plan.h"
#include "node_store.h"
#include "qt_core.h"
#include "signal_msg.h"
#include "signal_sched.h"
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

_Static_assert(SIGNAL_MSG_TYPE > _IO_MAX,
               "SIGNAL_MSG_TYPE is inside the reserved system message range");

enum qt_state { QT_INIT = 0, QT_NORMAL = 1, QT_SAFE = 2 };

static const char *state_name(enum qt_state s)
{
    switch (s) {
    case QT_NORMAL:
        return "NORMAL";
    case QT_SAFE:
        return "SAFE";
    default:
        return "INIT";
    }
}

/* ---- the arm, as in the Nerve sink (D-080, D-085, D-090) ---- */

#define ARM_MODEL_PRIO 15
#define ARM_MODEL_STEP_NS 2000000LL
#define ARM_COST_BUCKETS 1001
#define ARM_TRACK_PER_LINE 64

static struct arm_link g_arm_link;
static struct arm_drive g_drive;
static int g_arm_on;
static int g_arm_busy;
static unsigned g_arm_steps;
static unsigned g_arm_cost_us[ARM_COST_BUCKETS];
static int64_t g_arm_cost_max_ns;
static unsigned g_arm_urgent_in_step;
static unsigned g_arm_reports;
static struct arm_approach *g_approach;
static int64_t g_approach_published;

static int64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "sink: reading the monotonic clock failed: %s\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static long long run_ms(void)
{
    return (long long)(monotonic_ns() / 1000000LL);
}

static void write_stop(const struct arm_stop_report *r)
{
    char line[512];
    int n;

    n = snprintf(line, sizeof line,
                 "sink: arm-stop event_ms %lld delay_ns %lld issue_to_brake_ns "
                 "%lld speed_mm_s %.1f reaction_mm %.3f braking_mm %.3f "
                 "eye_event_mm %.2f eye_brake_mm %.2f eye_rest_mm %.2f "
                 "before_eye %s margin %s released %s\n",
                 (long long)(r->event_ns / 1000000LL),
                 (long long)(r->brake_ns - r->event_ns),
                 (long long)(r->brake_ns - r->issued_ns), r->speed_m_s * 1000.0,
                 r->reaction_m * 1000.0, r->braking_m * 1000.0,
                 r->eye_at_event_m * 1000.0, r->eye_at_brake_m * 1000.0,
                 r->eye_at_stop_m * 1000.0, r->before_eye ? "yes" : "no",
                 r->in_margin ? "yes" : "no", r->released_early ? "yes" : "no");
    if (n > 0 && (size_t)n < sizeof line) {
        (void)write(STDOUT_FILENO, line, (size_t)n);
    }
}

static unsigned arm_cost_median_us(unsigned steps)
{
    unsigned seen = 0;
    unsigned i;

    for (i = 0; i < ARM_COST_BUCKETS; i++) {
        seen += g_arm_cost_us[i];
        if (seen * 2 >= steps) {
            return i;
        }
    }
    return ARM_COST_BUCKETS - 1;
}

static void print_arm_summary(void)
{
    unsigned steps = __atomic_load_n(&g_arm_steps, __ATOMIC_RELAXED);

    if (!g_arm_on) {
        printf("sink: arm model off (SIGNAL_ARM=0)\n");
        return;
    }
    printf("sink: arm model steps %u, period %lld ns, priority %d, step median "
           "%u us, max %lld ns, over 1 ms %u, URGENT inside a step %u, person "
           "stops %u, state stops %u, stop reports %u\n",
           steps, (long long)ARM_MODEL_STEP_NS, ARM_MODEL_PRIO,
           steps > 0 ? arm_cost_median_us(steps) : 0,
           (long long)g_arm_cost_max_ns,
           g_arm_cost_us[ARM_COST_BUCKETS - 1], g_arm_urgent_in_step,
           g_drive.person_stops, g_drive.state_stops, g_arm_reports);
}

static void print_arm_track(void)
{
    const struct arm_track *tr = &g_drive.track;
    int end = __atomic_load_n(&tr->end, __ATOMIC_ACQUIRE);
    unsigned n = __atomic_load_n(&tr->count, __ATOMIC_ACQUIRE);
    unsigned i;

    if (!g_arm_on) {
        return;
    }
    if (n == 0) {
        printf("sink: arm-track none\n");
        return;
    }
    printf("sink: arm-track start_ns %lld step_ns %lld samples %u end %s "
           "brake_ns %lld brake_by %s stop_number %" PRIu64 " touch_ns %lld\n",
           (long long)tr->start_ns, (long long)ARM_TRACK_STEP_NS, n,
           arm_track_end_name(end),
           end == ARM_TRACK_OPEN ? 0LL : (long long)tr->brake_ns,
           arm_brake_reason_name(end == ARM_TRACK_OPEN ? ARM_BRAKE_NONE
                                                       : tr->brake_by),
           end == ARM_TRACK_OPEN ? (uint64_t)0 : tr->stop_number,
           end == ARM_TRACK_OPEN ? 0LL : (long long)tr->touch_ns);
    for (i = 0; i < n; i++) {
        if (i % ARM_TRACK_PER_LINE == 0) {
            printf("%ssink: arm-track-um %u", i == 0 ? "" : "\n", i);
        }
        printf(" %ld", (long)tr->eye_um[i]);
    }
    printf("\n");
}

static void *arm_thread(void *arg)
{
    struct sched_param param;
    struct timespec next;
    int64_t next_ns;

    (void)arg;
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "sink: pinning the arm thread to CPU 0 failed: %s\n",
                strerror(errno));
    }
    memset(&param, 0, sizeof param);
    param.sched_priority = ARM_MODEL_PRIO;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        fprintf(stderr, "sink: the arm thread could not take priority %d\n",
                ARM_MODEL_PRIO);
    }
    next_ns = monotonic_ns();
    for (;;) {
        struct arm_link_view view;
        struct arm_stop_report report;
        int64_t t0;
        int64_t cost;
        int done;
        unsigned us;

        next_ns += ARM_MODEL_STEP_NS;
        next.tv_sec = (time_t)(next_ns / 1000000000LL);
        next.tv_nsec = (long)(next_ns % 1000000000LL);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) ==
               EINTR) {
        }
        __atomic_store_n(&g_arm_busy, 1, __ATOMIC_RELAXED);
        t0 = monotonic_ns();
        arm_link_read(&g_arm_link, &view);
        done = arm_drive_update(&g_drive, &view, t0, &report);
        if (g_approach != NULL && g_drive.track.count > 0 &&
            g_drive.track.start_ns != g_approach_published) {
            g_approach_published = g_drive.track.start_ns;
            arm_approach_publish(g_approach, g_drive.track.start_ns,
                                 g_drive.track.eye_um[0]);
        }
        cost = monotonic_ns() - t0;
        __atomic_store_n(&g_arm_busy, 0, __ATOMIC_RELAXED);
        us = (unsigned)(cost / 1000);
        if (us >= ARM_COST_BUCKETS) {
            us = ARM_COST_BUCKETS - 1;
        }
        g_arm_cost_us[us]++;
        if (cost > g_arm_cost_max_ns) {
            g_arm_cost_max_ns = cost;
        }
        __atomic_store_n(&g_arm_steps, g_arm_steps + 1, __ATOMIC_RELAXED);
        if (done) {
            g_arm_reports++;
            write_stop(&report);
        }
        if (monotonic_ns() > next_ns + ARM_MODEL_STEP_NS) {
            next_ns = monotonic_ns();
        }
    }
    return NULL;
}

static void start_arm(void)
{
    const char *off = getenv("SIGNAL_ARM");
    enum node_store_found found;
    pthread_t thread;
    int rc;

    arm_link_init(&g_arm_link);
    arm_drive_init(&g_drive, monotonic_ns());
    g_approach = node_store_map(ARM_APPROACH_STORE, sizeof *g_approach, 0,
                                &found);
    if (g_approach == NULL) {
        fprintf(stderr, "sink: making %s failed: %s; no planned event "
                "can come\n", ARM_APPROACH_STORE, strerror(errno));
    }
    if (off != NULL && strcmp(off, "0") == 0) {
        printf("sink: arm model off (SIGNAL_ARM=0)\n");
        fflush(stdout);
        return;
    }
    rc = pthread_create(&thread, NULL, arm_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "sink: the arm thread did not start: %s; the run goes "
                "on without the arm\n", strerror(rc));
        return;
    }
    g_arm_on = 1;
    printf("sink: arm model every %lld ns at priority %d, speed %.0f mm/s, "
           "brake %.2f m/s^2, home %.1f mm from the eye, build %s\n",
           (long long)ARM_MODEL_STEP_NS, ARM_MODEL_PRIO, ARM_SPEED_M_S * 1000.0,
           ARM_DECEL_M_S2, arm_eye_distance(g_drive.arm.q) * 1000.0,
           QT_BUILD_NAME);
    fflush(stdout);
}

/* ---- what the sink saw ---- */

static unsigned g_received[3];
static unsigned g_late[3];
#define URGENT_RECEIVED_CAP 4096
#define URGENT_RECEIVED_PER_LINE 64
static uint64_t g_urgent_received[URGENT_RECEIVED_CAP];
static unsigned g_urgent_received_n;
static unsigned g_urgent_received_missed;

static void show_state(enum qt_state *shown, enum qt_state now)
{
    char line[96];

    if (*shown == now) {
        return;
    }
    *shown = now;
    arm_link_on_state(&g_arm_link, now == QT_NORMAL, monotonic_ns());
    if (qt_line_sink_state(line, sizeof line, state_name(now),
                           signal_log_quiet(), run_ms()) > 0) {
        fputs(line, stdout);
        fflush(stdout);
    }
}

/*
 * Counts only what arrived here; the fate of every issued number is
 * counted on the host from the source and sink logs (D-098 (7)).
 */
static void print_summary(void)
{
    unsigned i;

    for (i = 0; i < 3; i++) {
        printf("sink: summary %s received %u, on time %u, late %u\n",
               signal_class_name(i + 1), g_received[i],
               g_received[i] - g_late[i], g_late[i]);
    }
    for (i = 0; i < g_urgent_received_n; i++) {
        if (i % URGENT_RECEIVED_PER_LINE == 0) {
            printf("%ssink: urgent-received %u", i == 0 ? "" : "\n", i);
        }
        printf(" %" PRIu64, g_urgent_received[i]);
    }
    if (g_urgent_received_n > 0) {
        printf("\n");
    }
    printf("sink: urgent-received count %u not listed %u\n",
           g_urgent_received_n, g_urgent_received_missed);
    print_arm_summary();
    print_arm_track();
    fflush(stdout);
}

static int redirect(const char *path)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);

    if (fd == -1) {
        fprintf(stderr, "sink: opening %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

union sink_msg {
    struct _pulse pulse;
    uint16_t type;
    struct qt_end end;
    struct signal_message signal;
};

int main(int argc, char **argv)
{
    struct qt_opts opts;
    name_attach_t *attach;
    static union sink_msg buffer;
    enum qt_state state = QT_INIT;
    enum qt_state shown = QT_SAFE;
    unsigned long count;
    unsigned long gap_ms;
    int64_t gap_ns;
    int64_t last_urgent_ns;
    int first;
    int status = EXIT_SUCCESS;
    char line[256];

    first = qt_parse_opts(argc, argv, &opts);
    if (first < 0 || qt_parse_sink(argc, argv, first, &count, &gap_ms) != 0) {
        fprintf(stderr, "usage: sink [-o FILE] [-q] [-c 0] [-r] <signal-count> "
                "[urgent-gap-ms]\n");
        return EXIT_FAILURE;
    }
    if (opts.out != NULL && redirect(opts.out) != 0) {
        return EXIT_FAILURE;
    }
    if (opts.quiet) {
        setenv("SIGNAL_QUIET", "1", 1);
    }
    if (setvbuf(stdout, NULL, _IOFBF, 65536) != 0) {
        fprintf(stderr, "sink: could not size the output buffer\n");
    }
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "sink: pinning this thread to CPU 0 failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    gap_ns = (int64_t)gap_ms * 1000000LL;
    printf("sink: pid %d start_ns %lld, watchdog window %lld ns, build %s\n",
           (int)getpid(), (long long)monotonic_ns(), (long long)gap_ns,
           QT_BUILD_NAME);
    start_arm();
    show_state(&shown, state);

    attach = name_attach(NULL, SIGNAL_ATTACH_NAME, 0);
    if (attach == NULL) {
        fprintf(stderr, "sink: could not register the name %s: %s\n",
                SIGNAL_ATTACH_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    last_urgent_ns = monotonic_ns();
    for (;;) {
        struct _msg_info info;
        enum signal_verdict verdict;
        uint64_t timeout;
        rcvid_t rcvid;
        int64_t now_ns = monotonic_ns();
        int64_t left = last_urgent_ns + gap_ns - now_ns;
        uint32_t cls;

        if (left <= 0) {
            /* No URGENT within the window: the watchdog brakes the arm. */
            state = QT_SAFE;
            show_state(&shown, state);
            last_urgent_ns = now_ns;
            left = gap_ns;
        }
        timeout = (uint64_t)left;
        /* One-shot, cleared by the next kernel call: right before the receive. */
        if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout,
                         NULL) == -1) {
            fprintf(stderr, "sink: arming the watchdog failed: %s\n",
                    strerror(errno));
            status = EXIT_FAILURE;
            break;
        }
        memset(&info, 0, sizeof info);
        rcvid = MsgReceive(attach->chid, &buffer, sizeof buffer, &info);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT || errno == EINTR) {
                continue;
            }
            fprintf(stderr, "sink: waiting for a signal failed: %s\n",
                    strerror(errno));
            status = EXIT_FAILURE;
            break;
        }
        if (rcvid == 0) {
            if (buffer.pulse.code == _PULSE_CODE_DISCONNECT) {
                (void)ConnectDetach(buffer.pulse.scoid);
            } else if (buffer.pulse.code == _PULSE_CODE_UNBLOCK) {
                (void)MsgError(buffer.pulse.value.sival_int, EINTR);
            }
            continue;
        }
        if (info.msglen < (ssize_t)sizeof(uint16_t) || buffer.type <= _IO_MAX) {
            (void)MsgError(rcvid, ENOSYS);
            continue;
        }
        if (buffer.type == QT_MSG_END) {
            (void)MsgReply(rcvid, EOK, NULL, 0);
            printf("sink: end of the run\n");
            break;
        }
        if (buffer.type != SIGNAL_MSG_TYPE ||
            info.msglen < (ssize_t)sizeof(struct signal_message) ||
            buffer.signal.body.hop_count > SIGNAL_MAX_HOPS ||
            buffer.signal.body.class_id < SIGNAL_CLASS_URGENT ||
            buffer.signal.body.class_id > SIGNAL_CLASS_BACKGROUND) {
            (void)MsgError(rcvid, EINVAL);
            continue;
        }

        now_ns = monotonic_ns();
        cls = buffer.signal.body.class_id;
        /* The brake engages here, before the reply (D-080). */
        if (cls == SIGNAL_CLASS_URGENT) {
            if (__atomic_load_n(&g_arm_busy, __ATOMIC_RELAXED)) {
                g_arm_urgent_in_step++;
            }
            (void)arm_link_on_urgent(&g_arm_link, &buffer.signal.body, now_ns);
        } else if (cls == SIGNAL_CLASS_COMMAND) {
            arm_link_on_command(&g_arm_link, &buffer.signal.body);
        }
        verdict = signal_check(&buffer.signal.body, now_ns);
        if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
            fprintf(stderr, "sink: replying to the sender failed: %s\n",
                    strerror(errno));
        }
        g_received[cls - 1]++;
        if (verdict != SIGNAL_ON_TIME) {
            g_late[cls - 1]++;
        }
        if (cls == SIGNAL_CLASS_URGENT) {
            last_urgent_ns = now_ns;
            if (g_urgent_received_n < URGENT_RECEIVED_CAP) {
                g_urgent_received[g_urgent_received_n++] = buffer.signal.body.number;
            } else {
                g_urgent_received_missed++;
            }
            state = QT_NORMAL;
            show_state(&shown, state);
        }
        if (!signal_log_quiet() || verdict != SIGNAL_ON_TIME) {
            if (signal_log_quiet()) {
                printf("sink: at %lld ms ", run_ms());
            }
            if (qt_line_sink_signal(line, sizeof line, &buffer.signal.body,
                                    verdict, now_ns) > 0) {
                fputs(line, stdout);
            }
        }
        if (verdict != SIGNAL_ON_TIME &&
            qt_line_sink_late(line, sizeof line, &buffer.signal.body,
                              now_ns) > 0) {
            fputs(line, stdout);
            fflush(stdout);
        }
    }

    print_summary();
    if (name_detach(attach, 0) == -1) {
        fprintf(stderr, "sink: removing the name %s failed: %s\n",
                SIGNAL_ATTACH_NAME, strerror(errno));
    }
    if (node_store_remove(ARM_APPROACH_STORE) != 0) {
        fprintf(stderr, "sink: removing %s failed: %s\n", ARM_APPROACH_STORE,
                strerror(errno));
    }
    printf("sink: exit status %d\n", status == EXIT_SUCCESS ? 0 : 1);
    fflush(stdout);
    (void)count;
    return status;
}
