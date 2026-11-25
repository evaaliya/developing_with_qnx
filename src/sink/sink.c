#define _QNX_SOURCE

#include "arm_drive.h"
#include "issued_log.h"
#include "net_state.h"
#include "node_store.h"
#include "signal_ledger.h"
#include "signal_msg.h"
#include "telemetry_pub.h"
#include "telemetry_snap.h"
#include "wire.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
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
_Static_assert(SIGNAL_MANIFEST_CAP == SIGNAL_LEDGER_CAP,
               "the manifest and the ledger cover different numbers");

#define SAFETY_SECONDS 45u
#define DEFAULT_GAP_MS 1000ul

static const struct signal_ledger *pub_ledger;
static enum net_state current_state = NET_INIT;
/* 1 when a restarted sink found no books to go on from (D-063). */
static int books_reset;
static struct telem_window hop_window[3];
static int telemetry_on;

/*
 * The arm (D-080). The receive thread writes g_arm_link with plain
 * atomic stores and never waits for the model thread; the model thread
 * owns g_drive. The model runs at ARM_MODEL_PRIO on CPU 0: below URGENT
 * (30) and COMMAND (20), so a signal received there preempts it, and
 * above the BACKGROUND flood (12), so the arm keeps moving under it.
 */
#define ARM_MODEL_PRIO 15
#define ARM_MODEL_STEP_NS 2000000LL
/* One arm telemetry report every this many steps (50 ms). */
#define ARM_TELEM_EVERY 25
/* Step costs in microseconds; the last bucket is 1000 us or more. */
#define ARM_COST_BUCKETS 1001

static struct arm_link g_arm_link;
static struct arm_drive g_drive;
static int g_arm_on;
static int g_arm_busy;
static unsigned g_arm_steps;
static unsigned g_arm_cost_us[ARM_COST_BUCKETS];
static int64_t g_arm_cost_max_ns;
static unsigned g_arm_urgent_in_step;
static unsigned g_arm_reports;

/* Whole-run totals are kept by the ledger; no pass over the ring (D-062). */
static void class_counts(const struct signal_ledger *ledger, uint32_t id,
                         unsigned *on_time, unsigned *late, unsigned *lost,
                         unsigned *dropped)
{
    signal_ledger_counts(ledger, id, on_time, late, lost, dropped);
}

static void publish_sink(const struct signal_ledger *ledger,
                         enum net_state state, int flush)
{
    struct telem_class klass[3];
    struct telem_books books;
    char buf[TELEM_REPORT_CAP];
    int i;
    int n;

    if (!telemetry_on || ledger == NULL) {
        return;
    }
    memset(klass, 0, sizeof klass);
    for (i = 0; i < 3; i++) {
        unsigned on_time;
        unsigned late;
        unsigned lost;
        unsigned dropped;

        class_counts(ledger, (uint32_t)(i + 1), &on_time, &late, &lost,
                     &dropped);
        klass[i].on_time = on_time;
        klass[i].late = late;
        klass[i].lost = lost;
        klass[i].dropped = dropped;
        telem_class_set_latency(&klass[i], &hop_window[i]);
    }
    memset(&books, 0, sizeof books);
    books.sink_resumes = ledger->resumes;
    books.sink_reset = (uint32_t)books_reset;
    n = telem_report_sink(buf, sizeof buf, state, klass, &books);
    if (n < 0) {
        return;
    }
    if (flush) {
        telem_pub_flush(buf, (size_t)n);
        telemetry_on = 0;
        return;
    }
    telem_pub_post(buf, (size_t)n);
}

struct receive_buffer {
    union {
        struct _pulse pulse;
        struct signal_message message;
        struct signal_manifest manifest;
        struct signal_close close;
        struct signal_lock lock;
        struct signal_dropped dropped;
    } body;
};

static void on_alarm(int signo)
{
    (void)signo;
}

static int install_alarm(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof action);
    action.sa_handler = on_alarm;
    if (sigemptyset(&action.sa_mask) == -1 ||
        sigaction(SIGALRM, &action, NULL) == -1) {
        fprintf(stderr, "sink: could not arrange a receive timeout: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

static int pin_cpu0(void)
{
    /* Bit 0 is CPU 0. The mask is the pointer value, not a pointer. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "sink: pinning this thread to CPU 0 failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "sink: reading the monotonic clock failed: %s\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec > 999999999L ||
        ts.tv_sec > (INT64_MAX / 1000000000LL)) {
        fprintf(stderr,
                "sink: the monotonic clock value does not fit in a signed "
                "64-bit nanosecond count\n");
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void detach(name_attach_t *attach)
{
    if (attach != NULL && name_detach(attach, 0) == -1) {
        fprintf(stderr, "sink: removing the name %s failed: %s\n",
                SIGNAL_ATTACH_NAME, strerror(errno));
    }
}

static int reject(rcvid_t rcvid, int error, const char *what)
{
    if (MsgError(rcvid, error) == -1) {
        fprintf(stderr, "sink: %s: %s\n", what, strerror(errno));
        return -1;
    }
    return 0;
}

static int parse_count(const char *text, unsigned long *out, int allow_zero)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0) {
        return -1;
    }
    if (!allow_zero && value == 0) {
        return -1;
    }
    *out = value;
    return 0;
}

/*
 * CLOCK_MONOTONIC in milliseconds. Quiet lines carry it so they can be
 * matched with stall-probe lines from the same guest (B22).
 */
static long long run_ms(void)
{
    return (long long)(monotonic_ns() / 1000000LL);
}

/* NORMAL, DEGRADED and RECOVERING let the arm move; others brake it. */
static int state_lets_arm_move(enum net_state state)
{
    return state == NET_NORMAL || state == NET_DEGRADED ||
           state == NET_RECOVERING;
}

static void show_state(enum net_state *shown, enum net_state now)
{
    int ok = state_lets_arm_move(now);

    current_state = now;
    if (ok != __atomic_load_n(&g_arm_link.state_ok, __ATOMIC_RELAXED)) {
        arm_link_on_state(&g_arm_link, ok, monotonic_ns());
    }
    if (*shown != now) {
        *shown = now;
        if (signal_log_quiet()) {
            printf("sink: state %s at %lld ms\n", net_state_name(now), run_ms());
        } else {
            printf("sink: state %s\n", net_state_name(now));
        }
        fflush(stdout);
    }
    if (pub_ledger != NULL) {
        publish_sink(pub_ledger, now, 0);
    }
}

static void print_signal(const struct signal *sig, enum signal_verdict verdict,
                         enum net_presentation presentation, int64_t now_ns)
{
    int64_t prev = sig->issued_ns;
    uint32_t i;
    uint32_t hops = sig->hop_count;

    if (hops > SIGNAL_MAX_HOPS) {
        hops = SIGNAL_MAX_HOPS;
    }

    printf("sink: signal %" PRIu64 " class %s %s",
           sig->number, signal_class_name(sig->class_id),
           signal_verdict_name(verdict));
    if (presentation == NET_PRESENT_FRESH) {
        printf(", fresh");
    } else if (verdict == SIGNAL_ON_TIME) {
        printf(", held");
    }
    printf(", hops");
    for (i = 0; i < hops; i++) {
        printf(" %" PRId64 " ns,", sig->hop_ns[i] - prev);
        prev = sig->hop_ns[i];
    }
    printf(" %" PRId64 " ns, total %" PRId64 " ns\n",
           now_ns - prev, now_ns - sig->issued_ns);
    fflush(stdout);
}

/* Median step cost in microseconds from the histogram. */
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

/*
 * Read without a lock while the model may still step: a count may be
 * one step behind. The line shows that the model does not hold URGENT
 * up: its step cost and how many URGENT arrived inside a step (D-080).
 */
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

static void print_summary(const struct signal_ledger *ledger)
{
    static const uint32_t classes[] = {
        SIGNAL_CLASS_URGENT,
        SIGNAL_CLASS_COMMAND,
        SIGNAL_CLASS_BACKGROUND
    };
    size_t i;

    for (i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        uint32_t id = classes[i];
        unsigned received = 0;
        unsigned on_time = 0;
        unsigned late = 0;
        unsigned lost = 0;
        unsigned dropped = 0;

        class_counts(ledger, id, &on_time, &late, &lost, &dropped);
        received = on_time + late;
        printf("sink: summary %s received %u, on time %u, late %u, lost %u, "
               "dropped %u\n",
               signal_class_name(id), received, on_time, late, lost, dropped);
    }
    printf("sink: unaccounted %u\n", signal_ledger_unaccounted(ledger));
    print_arm_summary();
    fflush(stdout);
    publish_sink(ledger, current_state, 1);
}

/*
 * One line per lost number, flushed once per close by the caller. A
 * flush per line held the only sink thread for 111-128 ms on a close
 * with ~900 losses after a restart, and URGENT waited (D-063).
 */
static void print_lost(uint64_t number, uint32_t class_id, void *arg)
{
    (void)arg;
    printf("sink: lost signal %" PRIu64 " class %s\n",
           number, signal_class_name(class_id));
}

/*
 * Puts a list of issued numbers on the books (D-062). A repeated close
 * keeps the accounts it already gave (D-063). 0, or -1 with a message
 * when the ledger refused a number.
 */
static int close_items(struct signal_ledger *ledger,
                       const struct signal_manifest_item *items, unsigned n,
                       const char *what, unsigned *lost_out,
                       unsigned *dropped_out)
{
    struct signal_ledger_close_result res;
    int rc = signal_ledger_close(ledger, items, n, print_lost, NULL, &res);

    fflush(stdout);
    if (rc != 0) {
        fprintf(stderr,
                "sink: the %s repeats signal %" PRIu64 ", or its ledger "
                "slot still holds a number that is not closed\n",
                what, res.refused);
        return -1;
    }
    if (res.repeated > 0) {
        printf("sink: the %s repeats %u numbers already on the books; "
               "they keep their account\n", what, res.repeated);
    }
    if (res.drop_refused > 0) {
        printf("sink: the %s marks %u numbers dropped that arrived or are "
               "URGENT; they keep their account\n", what, res.drop_refused);
    }
    fflush(stdout);
    if (lost_out != NULL) {
        *lost_out = res.lost;
    }
    if (dropped_out != NULL) {
        *dropped_out = res.dropped;
    }
    return 0;
}

static int arm_receive(uint64_t gap_ns)
{
    /*
     * TimerTimeout is one-shot and is cleared by the next kernel call,
     * so it sits immediately in front of MsgReceive.
     */
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &gap_ns,
                     NULL) == -1) {
        fprintf(stderr, "sink: arming the urgent-pulse timeout failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

static void print_resumed(const struct signal_ledger *ledger)
{
    static const uint32_t classes[] = {
        SIGNAL_CLASS_URGENT, SIGNAL_CLASS_COMMAND, SIGNAL_CLASS_BACKGROUND
    };
    size_t i;

    printf("sink: resumed the books of a dead sink (resume %u)\n",
           (unsigned)ledger->resumes);
    for (i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        unsigned on_time;
        unsigned late;
        unsigned lost;
        unsigned dropped;

        class_counts(ledger, classes[i], &on_time, &late, &lost, &dropped);
        printf("sink: books kept %s on time %u, late %u, lost %u, dropped %u\n",
               signal_class_name(classes[i]), on_time, late, lost, dropped);
    }
    printf("sink: the network state and the latency window start again; "
           "a signal answered in the instant before the kill may be counted "
           "lost\n");
    fflush(stdout);
}

/*
 * The ledger lives in shared memory so a restarted sink goes on from
 * the dead one's totals (D-063). A sink started without -r begins new
 * books. If shared memory fails, the run goes on with books in this
 * process only, and that is said.
 */
static struct signal_ledger *open_books(int resume)
{
    static struct signal_ledger local;
    struct signal_ledger_resume r;
    enum node_store_found found = NODE_STORE_NEW;
    struct signal_ledger *ledger;

    ledger = node_store_map(SIGNAL_LEDGER_STORE, sizeof *ledger, resume,
                            &found);
    if (ledger == NULL) {
        fprintf(stderr, "sink: shared memory %s failed: %s; the books will "
                "not outlive this process\n", SIGNAL_LEDGER_STORE,
                strerror(errno));
        ledger = &local;
        found = resume ? NODE_STORE_MISSING : NODE_STORE_NEW;
    }
    if (found == NODE_STORE_KEPT && signal_ledger_resume(ledger, &r) == 0) {
        if (r.journal_redone) {
            printf("sink: finished a slot hand-over the dead sink began\n");
        }
        print_resumed(ledger);
        return ledger;
    }
    signal_ledger_init(ledger);
    if (resume) {
        books_reset = 1;
        printf("sink: books reset after a restart (%s); totals before this "
               "restart are not included\n",
               found == NODE_STORE_KEPT ? "saved books were not whole"
                                        : node_store_found_text(found));
        fflush(stdout);
    }
    return ledger;
}

/* One finished stop as one line, written whole without the stdio lock. */
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

static void publish_arm(void)
{
    struct telem_arm a;
    char buf[TELEM_REPORT_CAP];
    double speed = g_drive.arm.speed;
    double eye = arm_eye_distance(g_drive.arm.q);
    int i;
    int n;

    if (!telemetry_on) {
        return;
    }
    memset(&a, 0, sizeof a);
    a.have = 1;
    a.brake = g_drive.arm.brake;
    a.person = __atomic_load_n(&g_arm_link.person_in, __ATOMIC_RELAXED);
    for (i = 0; i < ARM_JOINTS; i++) {
        double m = g_drive.arm.q[i] * 1000.0;

        a.q_mrad[i] = (int32_t)(m < 0.0 ? m - 0.5 : m + 0.5);
    }
    a.eye_um = (uint32_t)(eye * 1e6 + 0.5);
    a.speed_um_s = (uint32_t)(speed * 1e6 + 0.5);
    a.person_stops = g_drive.person_stops;
    n = telem_report_arm(buf, sizeof buf, &a);
    if (n > 0) {
        telem_pub_post_aux(buf, (size_t)n);
    }
}

/*
 * The arm model on a fixed 2 ms step (D-080). Absolute wake times, so
 * the step does not drift. Physics uses the brake time the receive
 * thread stamped, not the step time, so a late step only catches up.
 */
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
        if (g_arm_steps % ARM_TELEM_EVERY == 0) {
            publish_arm();
        }
        /* A step that overran its period starts the next from now. */
        if (monotonic_ns() > next_ns + ARM_MODEL_STEP_NS) {
            next_ns = monotonic_ns();
        }
    }
    return NULL;
}

static void start_arm(void)
{
    const char *off = getenv("SIGNAL_ARM");
    pthread_t thread;
    int rc;

    arm_link_init(&g_arm_link);
    arm_drive_init(&g_drive, monotonic_ns());
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
           "brake %.2f m/s^2, home %.1f mm from the eye\n",
           (long long)ARM_MODEL_STEP_NS, ARM_MODEL_PRIO, ARM_SPEED_M_S * 1000.0,
           ARM_DECEL_M_S2, arm_eye_distance(g_drive.arm.q) * 1000.0);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    name_attach_t *attach;
    struct signal_ledger *ledger;
    struct net_machine machine;
    static struct receive_buffer buffer;
    enum net_state shown = NET_LOCKED_SAFE;
    unsigned long expected;
    unsigned long gap_ms = DEFAULT_GAP_MS;
    int64_t gap_ns;
    int finished = 0;
    int manifest_seen = 0;
    /* Default pin is the D-049 result: free runs stalled, pinned runs did not. */
    int pin = 1;
    int arg0 = 1;
    int resume = 0;

    while (arg0 < argc && argv[arg0][0] == '-') {
        if (strcmp(argv[arg0], "-c") == 0 && arg0 + 1 < argc &&
            strcmp(argv[arg0 + 1], "0") == 0) {
            pin = 1;
            arg0 += 2;
        } else if (strcmp(argv[arg0], "-r") == 0) {
            resume = 1;
            arg0++;
        } else {
            fprintf(stderr, "usage: sink [-c 0] [-r] <signal-count> [urgent-gap-ms]\n");
            return EXIT_FAILURE;
        }
    }
    if ((argc - arg0 != 1 && argc - arg0 != 2) ||
        parse_count(argv[arg0], &expected, 0) != 0 ||
        (argc - arg0 == 2 && parse_count(argv[arg0 + 1], &gap_ms, 0) != 0)) {
        fprintf(stderr, "usage: sink [-c 0] [-r] <signal-count> [urgent-gap-ms]\n");
        return EXIT_FAILURE;
    }
    if (expected > SIGNAL_LEDGER_CAP) {
        fprintf(stderr, "sink: the count must be 1..%d\n", SIGNAL_LEDGER_CAP);
        return EXIT_FAILURE;
    }
    /*
     * The count is only a limit check. The loop ends when the manifest
     * arrives. Numbers the relay marked as dropped are closed from that
     * manifest, so they do not have to show up as messages.
     */

    /* Lost lines are flushed once per close (D-063). */
    if (setvbuf(stdout, NULL, _IOFBF, 65536) != 0) {
        fprintf(stderr, "sink: could not size the output buffer\n");
    }
    if (install_alarm() == -1) {
        return EXIT_FAILURE;
    }
    if (pin && pin_cpu0() != 0) {
        return EXIT_FAILURE;
    }

    ledger = open_books(resume);
    start_arm();
    gap_ns = (int64_t)gap_ms * 1000000LL;
    net_machine_init(&machine, monotonic_ns(), gap_ns, NET_RECOVER_URGENT_NEED);
    {
        int i;

        for (i = 0; i < 3; i++) {
            telem_window_init(&hop_window[i]);
        }
    }
    if (telem_pub_open("sink")) {
        telemetry_on = 1;
        pub_ledger = ledger;
    }
    show_state(&shown, machine.state);

    attach = name_attach(NULL, SIGNAL_ATTACH_NAME, 0);
    if (attach == NULL) {
        fprintf(stderr, "sink: could not register the name %s: %s\n",
                SIGNAL_ATTACH_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    alarm(SAFETY_SECONDS);
    while (!finished) {
        struct _msg_info info;
        rcvid_t rcvid;
        enum signal_verdict verdict;
        enum net_presentation presentation = NET_PRESENT_HELD;
        int64_t now_ns;

        now_ns = monotonic_ns();
        if (now_ns > machine.next_urgent_due_ns) {
            show_state(&shown, net_machine_on_tick(&machine, now_ns));
        }
        if (arm_receive((uint64_t)gap_ns) != 0) {
            print_summary(ledger);
            detach(attach);
            return EXIT_FAILURE;
        }

        memset(&buffer, 0, sizeof buffer);
        memset(&info, 0, sizeof info);
        rcvid = MsgReceive(attach->chid, &buffer, sizeof buffer, &info);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            if (errno == EINTR) {
                fprintf(stderr,
                        "sink: stopped, nothing closed the books for %u seconds\n",
                        SAFETY_SECONDS);
            } else {
                fprintf(stderr, "sink: waiting for a signal failed: %s\n",
                        strerror(errno));
            }
            print_summary(ledger);
            detach(attach);
            return EXIT_FAILURE;
        }

        if (rcvid == 0) {
            if (buffer.body.pulse.code == _PULSE_CODE_DISCONNECT) {
                if (ConnectDetach(buffer.body.pulse.scoid) == -1) {
                    fprintf(stderr,
                            "sink: detaching a departed client failed: %s\n",
                            strerror(errno));
                }
            } else if (buffer.body.pulse.code == _PULSE_CODE_UNBLOCK) {
                if (MsgError(buffer.body.pulse.value.sival_int, EINTR) == -1) {
                    fprintf(stderr,
                            "sink: releasing a client that asked to unblock "
                            "failed: %s\n",
                            strerror(errno));
                }
            }
            continue;
        }

        if (info.msglen < (ssize_t)sizeof(uint16_t) ||
            buffer.body.message.type <= _IO_MAX) {
            if (reject(rcvid, ENOSYS, "rejecting a system message failed") == -1) {
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.body.message.type == SIGNAL_MSG_DROPPED &&
            info.msglen >= (ssize_t)sizeof(struct signal_dropped)) {
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "sink: replying to a drop notice failed: %s\n",
                        strerror(errno));
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            show_state(&shown,
                       net_machine_on_dropped(&machine,
                                              buffer.body.dropped.class_id));
            continue;
        }

        if (buffer.body.message.type == SIGNAL_MSG_LOCK &&
            info.msglen >= (ssize_t)sizeof(struct signal_lock)) {
            show_state(&shown, net_machine_on_lock(&machine));
            if (!manifest_seen) {
                printf("sink: manifest did not arrive; this summary is only "
                       "signals sink saw\n");
            }
            print_summary(ledger);
            fflush(stdout);
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "sink: replying to the lock failed: %s\n",
                        strerror(errno));
                detach(attach);
                return EXIT_FAILURE;
            }
            detach(attach);
            alarm(0);
            return EXIT_SUCCESS;
        }

        if (buffer.body.message.type == SIGNAL_MSG_CLOSE &&
            info.msglen >= (ssize_t)sizeof(struct signal_close)) {
            unsigned lost = 0;
            unsigned dropped = 0;
            unsigned n = buffer.body.close.count;
            int64_t close_start;

            if (n == 0 || n > SIGNAL_CLOSE_CAP) {
                if (reject(rcvid, EINVAL, "rejecting a close failed") == -1) {
                    print_summary(ledger);
                    detach(attach);
                    return EXIT_FAILURE;
                }
                continue;
            }
            close_start = monotonic_ns();
            if (close_items(ledger, buffer.body.close.item, n, "close", &lost,
                            &dropped) != 0) {
                (void)reject(rcvid, EINVAL, "rejecting a close failed");
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "sink: replying to a close failed: %s\n",
                        strerror(errno));
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            /* The books moved, so the run is not stuck (D-062). */
            alarm(SAFETY_SECONDS);
            printf("sink: closed %u signals %" PRIu64 "..%" PRIu64
                   ", lost %u, dropped %u, open %u, took %lld ns\n",
                   n, buffer.body.close.item[0].number,
                   buffer.body.close.item[n - 1].number, lost, dropped,
                   signal_ledger_unaccounted(ledger),
                   (long long)(monotonic_ns() - close_start));
            fflush(stdout);
            show_state(&shown, machine.state);
            continue;
        }

        if (buffer.body.message.type == SIGNAL_MSG_MANIFEST &&
            info.msglen >= (ssize_t)sizeof(struct signal_manifest)) {
            unsigned item;

            if (buffer.body.manifest.count > SIGNAL_MANIFEST_CAP) {
                if (reject(rcvid, EINVAL, "rejecting the manifest failed") == -1) {
                    print_summary(ledger);
                    detach(attach);
                    return EXIT_FAILURE;
                }
                continue;
            }
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "sink: replying to the manifest failed: %s\n",
                        strerror(errno));
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            if (close_items(ledger, buffer.body.manifest.item,
                            buffer.body.manifest.count, "manifest", NULL,
                            &item) != 0) {
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            manifest_seen = 1;
            if (item > 0) {
                unsigned i;

                for (i = 0; i < buffer.body.manifest.count; i++) {
                    if (buffer.body.manifest.item[i].reserved ==
                        SIGNAL_MANIFEST_DROPPED) {
                        show_state(&shown, net_machine_on_dropped(
                                       &machine,
                                       buffer.body.manifest.item[i].class_id));
                    }
                }
            }
            if (issued_list_is_rebuilt(buffer.body.manifest.reserved)) {
                printf("sink: %s\n", issued_list_rebuilt_sentence());
                fflush(stdout);
            }
            {
                unsigned missed = signal_ledger_arrived_unissued(ledger);

                if (missed > 0) {
                    printf("sink: issued list did not include %u arrivals\n",
                           missed);
                    fflush(stdout);
                }
            }
            if (signal_ledger_unaccounted(ledger) != 0) {
                fprintf(stderr, "sink: the manifest left a signal unaccounted\n");
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            finished = 1;
            continue;
        }

        if (buffer.body.message.type != SIGNAL_MSG_TYPE ||
            info.msglen < (ssize_t)sizeof(struct signal_message) ||
            buffer.body.message.body.hop_count > SIGNAL_MAX_HOPS) {
            fprintf(stderr,
                    "sink: the message is not a signal (type %u, %lld bytes, "
                    "hop count %u)\n",
                    buffer.body.message.type, (long long)info.msglen,
                    buffer.body.message.body.hop_count);
            if (reject(rcvid, EINVAL, "rejecting that message failed") == -1) {
                print_summary(ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

        now_ns = monotonic_ns();
        /*
         * The brake engages here, before the reply: an atomic store, no
         * lock, no print (D-080). A late zone URGENT still brakes.
         */
        if (buffer.body.message.body.class_id == SIGNAL_CLASS_URGENT) {
            if (__atomic_load_n(&g_arm_busy, __ATOMIC_RELAXED)) {
                g_arm_urgent_in_step++;
            }
            (void)arm_link_on_urgent(&g_arm_link, &buffer.body.message.body,
                                     now_ns);
        } else if (buffer.body.message.body.class_id == SIGNAL_CLASS_COMMAND) {
            arm_link_on_command(&g_arm_link, &buffer.body.message.body);
        }
        verdict = signal_check(&buffer.body.message.body, now_ns);
        if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
            fprintf(stderr, "sink: replying to the sender failed: %s\n",
                    strerror(errno));
            print_summary(ledger);
            detach(attach);
            return EXIT_FAILURE;
        }

        if (signal_ledger_receive(ledger, buffer.body.message.body.number,
                                  buffer.body.message.body.class_id,
                                  verdict) != 0) {
            fprintf(stderr, "sink: signal %" PRIu64 " was already accounted\n",
                    buffer.body.message.body.number);
            continue;
        }
        if (telemetry_on &&
            buffer.body.message.body.class_id >= SIGNAL_CLASS_URGENT &&
            buffer.body.message.body.class_id <= SIGNAL_CLASS_BACKGROUND) {
            int64_t latency = now_ns - buffer.body.message.body.issued_ns;

            if (latency < 0) {
                latency = 0;
            }
            telem_window_add(&hop_window[buffer.body.message.body.class_id - 1],
                             latency);
        }
        net_machine_on_signal(&machine, buffer.body.message.body.class_id,
                              verdict, now_ns, &presentation);
        /* Quiet runs still print every late signal, with the run time. */
        if (!signal_log_quiet() || verdict != SIGNAL_ON_TIME) {
            if (signal_log_quiet()) {
                printf("sink: at %lld ms ", run_ms());
            }
            print_signal(&buffer.body.message.body, verdict, presentation,
                         now_ns);
        }
        /*
         * One line per late signal in both log modes, with its arrival on
         * CLOCK_MONOTONIC, so qnx-late-check.sh can hold it against the
         * stall probe and the check's own faults (D-064).
         */
        if (verdict != SIGNAL_ON_TIME) {
            printf("sink: late signal %" PRIu64 " class %s at %lld ms, "
                   "total %" PRId64 " ns\n",
                   buffer.body.message.body.number,
                   signal_class_name(buffer.body.message.body.class_id),
                   (long long)(now_ns / 1000000LL),
                   now_ns - buffer.body.message.body.issued_ns);
            fflush(stdout);
        }
        show_state(&shown, machine.state);
    }

    alarm(0);
    print_summary(ledger);
    detach(attach);
    /* The run is over and a clean exit is not restarted: free the books. */
    if (node_store_remove(SIGNAL_LEDGER_STORE) != 0) {
        fprintf(stderr, "sink: removing %s failed: %s\n", SIGNAL_LEDGER_STORE,
                strerror(errno));
    }
    return EXIT_SUCCESS;
}
