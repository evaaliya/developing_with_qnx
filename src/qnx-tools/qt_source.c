/*
 * source of the "QNX tools" network (D-098 (2), (7), (8)).
 *
 * One thread per class on a fixed SCHED_FIFO priority: URGENT 30,
 * COMMAND 20, BACKGROUND 12. Each waits for the pulse of its own timer
 * (timer_create with SIGEV_PULSE_INIT at the class priority) on its own
 * channel and sends each signal with MsgSend to the next node found by
 * name_open. Every send is limited by TimerTimeout to the class deadline
 * (D-058); on ETIMEDOUT the thread logs the line and goes on to its next
 * signal: no retry, no drop rule, no accounting in the network. On a
 * link error it logs and opens the name again before its next send.
 *
 * The scene is the same as in the Nerve source (D-098 (7)): the command
 * path /dev/signal-source, the schedule periods, the flood of 72
 * BACKGROUND every 50 ms (D-060), the arm payload and the planned
 * "person in" (D-089, D-090). Those parts are copied from
 * src/source/source.c; the Nerve books, closes, manifest, priority
 * raises and the stuck-send watch are not here.
 *
 *   source [-o FILE] [-q] live <next-name>
 */
#define _QNX_SOURCE

#include "arm_plan.h"
#include "qt_core.h"
#include "signal_msg.h"
#include "signal_sched.h"
#include "source_ctl.h"
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* sys/iofunc.h before sys/dispatch.h: it defines RESMGR_OCB_T as iofunc_ocb_t. */
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>

_Static_assert(SIGNAL_MSG_TYPE > _IO_MAX,
               "SIGNAL_MSG_TYPE is inside the reserved system message range");
_Static_assert(QT_MSG_END > _IO_MAX,
               "QT_MSG_END is inside the reserved system message range");

/* The device thread, the class threads and the zone sensor share these. */
static struct source_control g_ctl;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* The person-in and URGENT lists; printed after stop. */
static pthread_mutex_t g_list_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_t *g_dpp;
static const char *g_next_name;
static uint64_t g_next_number;
static int g_stop;

#define PULSE_TICK _PULSE_CODE_MINAVAIL
#define PULSE_ZONE (_PULSE_CODE_MINAVAIL + 1)
#define PULSE_ARM (_PULSE_CODE_MINAVAIL + 2)
#define PULSE_QUIT (_PULSE_CODE_MINAVAIL + 3)

#define FIND_ATTEMPTS 40
#define FIND_PAUSE_NS 50000000L
#define END_TIMEOUT_NS 5000000000ULL
#define JOIN_LIMIT_NS 2000000000LL
/* The BACKGROUND thread checks its schedule and the flood on this tick. */
#define BACKGROUND_TICK_NS 10000000LL

/* One class: its thread, its channel, its timer, its own connection. */
struct klass {
    uint32_t id;
    int prio;
    /* Timer period: the class period, or the BACKGROUND tick. */
    int64_t tick_ns;
    int chid;
    int self_coid;
    timer_t timer;
    int timer_made;
    int next_coid;
    pthread_t thread;
    int started;
    int done;
    unsigned long timeouts;
    unsigned long failures;
};

static struct klass g_class[3];

static void ctl_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void ctl_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static int stopping(void)
{
    return __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE);
}

static int set_fifo(int priority)
{
    struct sched_param param;
    int rc;

    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        fprintf(stderr, "source: setting priority %d failed: %s\n",
                priority, strerror(rc));
        return -1;
    }
    return 0;
}

static int pin_cpu0(void)
{
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "source: pinning this thread to CPU 0 failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

static int64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "source: reading the monotonic clock failed: %s\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static void sleep_until(int64_t t_ns)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(t_ns / 1000000000LL);
    ts.tv_nsec = (long)(t_ns % 1000000000LL);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static void print_line(const char *line, int n)
{
    if (n > 0) {
        fputs(line, stdout);
        fflush(stdout);
    }
}

static int open_next(const char *name)
{
    int attempt;

    for (attempt = 0; attempt < FIND_ATTEMPTS; attempt++) {
        int coid = name_open(name, 0);

        if (coid != -1) {
            return coid;
        }
        if (errno != ENOENT) {
            fprintf(stderr, "source: opening the next name %s failed: %s\n",
                    name, strerror(errno));
            return -1;
        }
        sleep_until(monotonic_ns() + FIND_PAUSE_NS);
    }
    fprintf(stderr,
            "source: could not find a node registered as %s after 2 seconds\n",
            name);
    return -1;
}

/* ---- person-in and URGENT lists, as in the Nerve source ---- */

#define PERSON_URGENT_CAP 128
#define PERSON_EVENT_CAP 8
static uint64_t g_person_urgent[PERSON_URGENT_CAP];
static unsigned g_person_urgent_n;
static unsigned g_person_urgent_missed;
static uint64_t g_person_event_number[PERSON_EVENT_CAP];
static int64_t g_person_event_ns[PERSON_EVENT_CAP];
static unsigned g_person_event_n;

#define URGENT_ISSUED_CAP 2048
#define URGENT_ISSUED_PER_LINE 64
static uint64_t g_urgent_issued[URGENT_ISSUED_CAP];
static unsigned g_urgent_issued_n;
static unsigned g_urgent_issued_missed;

static void note_urgent(uint64_t number, const struct signal_arm *arm)
{
    pthread_mutex_lock(&g_list_lock);
    if ((arm->flags & SIGNAL_ARM_PERSON_IN) != 0) {
        if ((arm->flags & SIGNAL_ARM_EVENT) != 0 &&
            g_person_event_n < PERSON_EVENT_CAP) {
            g_person_event_number[g_person_event_n] = number;
            g_person_event_ns[g_person_event_n] = arm->event_ns;
            g_person_event_n++;
        }
        if (g_person_urgent_n < PERSON_URGENT_CAP) {
            g_person_urgent[g_person_urgent_n++] = number;
        } else {
            g_person_urgent_missed++;
        }
    }
    if (g_urgent_issued_n < URGENT_ISSUED_CAP) {
        g_urgent_issued[g_urgent_issued_n++] = number;
    } else {
        g_urgent_issued_missed++;
    }
    pthread_mutex_unlock(&g_list_lock);
}

static void print_person_urgent(void)
{
    char line[96];
    unsigned i;

    for (i = 0; i < g_person_event_n; i++) {
        if (qt_line_person_event(line, sizeof line, g_person_event_ns[i],
                                 g_person_event_number[i]) > 0) {
            fputs(line, stdout);
        }
    }
    if (g_person_urgent_n == 0) {
        return;
    }
    printf("source: person-in URGENT");
    for (i = 0; i < g_person_urgent_n; i++) {
        printf(" %" PRIu64, g_person_urgent[i]);
    }
    printf("\n");
    if (g_person_urgent_missed > 0) {
        printf("source: person-in URGENT not listed %u\n", g_person_urgent_missed);
    }
}

static void print_urgent_issued(void)
{
    unsigned i;

    for (i = 0; i < g_urgent_issued_n; i++) {
        if (i % URGENT_ISSUED_PER_LINE == 0) {
            printf("%ssource: urgent-issued %u", i == 0 ? "" : "\n", i);
        }
        printf(" %" PRIu64, g_urgent_issued[i]);
    }
    if (g_urgent_issued_n > 0) {
        printf("\n");
    }
    printf("source: urgent-issued count %u not listed %u\n", g_urgent_issued_n,
           g_urgent_issued_missed);
}

/* ---- one send ---- */

static const char *link_errors_reopen(int err)
{
    /* The node behind this connection is gone; open the name again. */
    return (err == ESRCH || err == EBADF || err == ENOTCONN) ? "reopen" : NULL;
}

/*
 * Issues the next number of this class and sends it once, limited by
 * the class deadline (D-098 (8)). 0 when it went through, 1 when the
 * send timed out or failed (the line is printed), -1 on an error in
 * this process.
 */
static int issue(struct klass *k, uint32_t extra_flags)
{
    struct signal_message message;
    struct signal_reply reply;
    struct signal_arm arm;
    uint64_t number;
    uint64_t limit;
    int64_t t0;
    char line[160];
    long sent;

    number = __atomic_add_fetch(&g_next_number, 1, __ATOMIC_ACQ_REL);
    ctl_lock();
    source_ctl_note_issued(&g_ctl, k->id);
    source_ctl_arm_payload(&g_ctl, k->id, &arm);
    ctl_unlock();
    arm.flags |= extra_flags;
    if (k->id == SIGNAL_CLASS_URGENT) {
        note_urgent(number, &arm);
    }

    if (k->next_coid == -1) {
        k->next_coid = name_open(g_next_name, 0);
        if (k->next_coid == -1) {
            k->failures++;
            print_line(line, qt_line_send_failed(line, sizeof line, k->id,
                                                 number, errno));
            return 1;
        }
    }

    memset(&message, 0, sizeof message);
    message.type = SIGNAL_MSG_TYPE;
    message.body.number = number;
    message.body.class_id = k->id;
    message.body.deadline_ns = signal_class_deadline_ns(k->id);
    message.body.arm = arm;
    memset(&reply, 0, sizeof reply);
    limit = (uint64_t)qt_send_limit_ns(k->id);
    t0 = monotonic_ns();
    message.body.issued_ns = t0;
    /* TimerTimeout is one-shot and the next kernel call clears it: last. */
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &limit, NULL) == -1) {
        fprintf(stderr, "source: arming the send time limit failed: %s\n",
                strerror(errno));
        return -1;
    }
    sent = MsgSend(k->next_coid, &message, sizeof message, &reply, sizeof reply);
    if (sent == -1) {
        int err = errno;

        if (err == ETIMEDOUT) {
            k->timeouts++;
            print_line(line, qt_line_send_timeout(line, sizeof line, k->id,
                                                  number, monotonic_ns() - t0));
            return 1;
        }
        k->failures++;
        print_line(line, qt_line_send_failed(line, sizeof line, k->id, number,
                                             err));
        if (link_errors_reopen(err) != NULL) {
            (void)name_close(k->next_coid);
            k->next_coid = -1;
        }
        return 1;
    }
    if (sent != EOK) {
        k->failures++;
        print_line(line, qt_line_send_failed(line, sizeof line, k->id, number,
                                             (int)sent));
        return 1;
    }
    if (!signal_log_quiet()) {
        printf("source: sent signal %" PRIu64 " class %s\n", number,
               signal_class_name(k->id));
        fflush(stdout);
    }
    return 0;
}

/* ---- the class threads ---- */

static int start_timer(struct klass *k, int64_t first_ns)
{
    struct itimerspec spec;

    memset(&spec, 0, sizeof spec);
    spec.it_value.tv_sec = (time_t)(first_ns / 1000000000LL);
    spec.it_value.tv_nsec = (long)(first_ns % 1000000000LL);
    spec.it_interval.tv_sec = (time_t)(k->tick_ns / 1000000000LL);
    spec.it_interval.tv_nsec = (long)(k->tick_ns % 1000000000LL);
    if (timer_settime(k->timer, 0, &spec, NULL) == -1) {
        fprintf(stderr, "source: starting the %s timer failed: %s\n",
                signal_class_name(k->id), strerror(errno));
        return -1;
    }
    return 0;
}

/* The BACKGROUND tick: the ordinary schedule, then the flood (D-060). */
static void background_tick(struct klass *k, int64_t *next_ns,
                            int64_t *flood_next_ns)
{
    int64_t now = monotonic_ns();
    unsigned batch;
    unsigned i;
    int flood;

    if (*next_ns == 0) {
        *next_ns = now + SOURCE_BACKGROUND_PERIOD_NS;
    } else if (now + BACKGROUND_TICK_NS / 2 >= *next_ns) {
        (void)issue(k, 0);
        /* The previous time plus the period; passed times are skipped (D-081). */
        *next_ns += SOURCE_BACKGROUND_PERIOD_NS;
        while (*next_ns + SOURCE_BACKGROUND_PERIOD_NS <= now) {
            *next_ns += SOURCE_BACKGROUND_PERIOD_NS;
        }
    }
    ctl_lock();
    flood = g_ctl.flood;
    ctl_unlock();
    if (!flood) {
        *flood_next_ns = 0;
        return;
    }
    batch = source_flood_due(1, monotonic_ns(), flood_next_ns);
    /* 72 signals, each its own MsgSend under the BACKGROUND limit. */
    for (i = 0; i < batch && !stopping(); i++) {
        (void)issue(k, 0);
    }
}

static void *class_main(void *arg)
{
    struct klass *k = arg;
    int64_t next_ns = 0;
    int64_t flood_next_ns = 0;

    (void)pin_cpu0();
    if (set_fifo(k->prio) != 0) {
        __atomic_store_n(&k->done, 1, __ATOMIC_RELEASE);
        return NULL;
    }
    k->next_coid = open_next(g_next_name);
    for (;;) {
        struct _pulse pulse;
        rcvid_t rcvid;

        rcvid = MsgReceive(k->chid, &pulse, sizeof pulse, NULL);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "source: the %s thread could not wait for its "
                    "timer: %s\n", signal_class_name(k->id), strerror(errno));
            break;
        }
        if (rcvid != 0) {
            (void)MsgError(rcvid, ENOSYS);
            continue;
        }
        if (pulse.code == PULSE_QUIT || stopping()) {
            break;
        }
        if (pulse.code == PULSE_ZONE && k->id == SIGNAL_CLASS_URGENT) {
            /* A zone change goes now; the schedule keeps its times. */
            (void)issue(k, SIGNAL_ARM_EVENT);
            continue;
        }
        if (pulse.code == PULSE_ARM && k->id == SIGNAL_CLASS_COMMAND) {
            /* An arm command goes now; its schedule starts again from now. */
            (void)issue(k, 0);
            (void)start_timer(k, k->tick_ns);
            continue;
        }
        if (pulse.code != PULSE_TICK) {
            continue;
        }
        if (k->id == SIGNAL_CLASS_BACKGROUND) {
            background_tick(k, &next_ns, &flood_next_ns);
            continue;
        }
        if (k->id == SIGNAL_CLASS_URGENT) {
            int on;

            ctl_lock();
            on = g_ctl.urgent_on;
            ctl_unlock();
            if (!on) {
                continue;
            }
        }
        (void)issue(k, 0);
    }
    __atomic_store_n(&k->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int make_class(struct klass *k, uint32_t id, int prio, int64_t tick_ns)
{
    struct sigevent event;

    memset(k, 0, sizeof *k);
    k->id = id;
    k->prio = prio;
    k->tick_ns = tick_ns;
    k->next_coid = -1;
    k->chid = ChannelCreate(0);
    if (k->chid == -1) {
        fprintf(stderr, "source: creating the %s channel failed: %s\n",
                signal_class_name(id), strerror(errno));
        return -1;
    }
    k->self_coid = ConnectAttach(0, 0, k->chid, _NTO_SIDE_CHANNEL, 0);
    if (k->self_coid == -1) {
        fprintf(stderr, "source: attaching the %s channel failed: %s\n",
                signal_class_name(id), strerror(errno));
        return -1;
    }
    memset(&event, 0, sizeof event);
    SIGEV_PULSE_INIT(&event, k->self_coid, prio, PULSE_TICK, 0);
    if (MsgRegisterEvent(&event, k->self_coid) == -1) {
        fprintf(stderr, "source: registering the %s pulse failed: %s\n",
                signal_class_name(id), strerror(errno));
        return -1;
    }
    if (timer_create(CLOCK_MONOTONIC, &event, &k->timer) == -1) {
        fprintf(stderr, "source: creating the %s timer failed: %s\n",
                signal_class_name(id), strerror(errno));
        return -1;
    }
    k->timer_made = 1;
    return 0;
}

static void pulse_class(uint32_t id, int code)
{
    struct klass *k = &g_class[id - 1];

    if (k->self_coid > 0 && MsgSendPulse(k->self_coid, k->prio, code, 0) == -1) {
        fprintf(stderr, "source: waking the %s thread failed: %s\n",
                signal_class_name(id), strerror(errno));
    }
}

/* ---- the zone sensor (D-089, D-090), copied from the Nerve source ---- */

#define PLAN_WAIT_NS 3000000000LL
#define PLAN_POLL_NS 1000000LL
static sem_t g_plan_sem;
static int g_plan_started;
static int64_t g_plan_offset_ns;
static int64_t g_source_start_ns;
static int g_plan_done;
static int64_t g_plan_start_ns;
static int32_t g_plan_start_eye_um;
static int64_t g_plan_event_ns;
static int64_t g_plan_woke_ns;
static char g_plan_error[128];

static void plan_fail(const char *why)
{
    snprintf(g_plan_error, sizeof g_plan_error, "%s", why);
    __atomic_store_n(&g_plan_done, -1, __ATOMIC_RELEASE);
}

static void plan_once(void)
{
    const struct arm_approach *a = NULL;
    int64_t asked = monotonic_ns();
    int64_t start = 0;
    int32_t eye_um = 0;
    int64_t event;
    unsigned wake;
    int status;

    for (;;) {
        int64_t now = monotonic_ns();

        if (a == NULL) {
            a = arm_approach_open(ARM_APPROACH_STORE);
        }
        if (a != NULL) {
            int r = arm_approach_read(a, &start, &eye_um);

            if (r < 0) {
                arm_approach_close(a);
                plan_fail("the approach object is not the sink's");
                return;
            }
            if (r == 1 && start > g_source_start_ns) {
                break;
            }
        }
        if (now - asked > PLAN_WAIT_NS) {
            arm_approach_close(a);
            plan_fail(a == NULL ? "no approach object from the sink within 3 s"
                                : "no approach began within 3 s");
            return;
        }
        sleep_until(now + PLAN_POLL_NS);
    }
    arm_approach_close(a);
    event = start + g_plan_offset_ns;
    if (monotonic_ns() >= event) {
        g_plan_start_ns = start;
        g_plan_start_eye_um = eye_um;
        plan_fail("the planned point had passed when the approach was seen");
        return;
    }
    sleep_until(event);
    g_plan_woke_ns = monotonic_ns();

    ctl_lock();
    g_ctl.wake = 0;
    status = source_ctl_apply_at(&g_ctl, "person in", event);
    wake = g_ctl.wake;
    g_ctl.wake = 0;
    ctl_unlock();
    if ((wake & SOURCE_WAKE_URGENT) != 0) {
        pulse_class(SIGNAL_CLASS_URGENT, PULSE_ZONE);
    }
    g_plan_start_ns = start;
    g_plan_start_eye_um = eye_um;
    g_plan_event_ns = event;
    __atomic_store_n(&g_plan_done, status == 0 && (wake & SOURCE_WAKE_URGENT) != 0
                                       ? 1 : -1, __ATOMIC_RELEASE);
    if (status != 0 || (wake & SOURCE_WAKE_URGENT) == 0) {
        snprintf(g_plan_error, sizeof g_plan_error, "a person was already in");
    }
}

static void *plan_thread(void *arg)
{
    (void)arg;
    (void)pin_cpu0();
    if (set_fifo(SIGNAL_PRIO_URGENT) != 0) {
        return NULL;
    }
    for (;;) {
        if (sem_wait(&g_plan_sem) == -1) {
            if (errno == EINTR) {
                continue;
            }
            return NULL;
        }
        plan_once();
    }
}

static void start_plan(void)
{
    pthread_t thread;
    int rc;

    g_source_start_ns = monotonic_ns();
    g_plan_offset_ns = arm_plan_time_to_distance_ns(arm_plan_event_distance_m());
    if (sem_init(&g_plan_sem, 0, 0) == -1) {
        fprintf(stderr, "source: the zone sensor semaphore failed: %s\n",
                strerror(errno));
        return;
    }
    rc = pthread_create(&thread, NULL, plan_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "source: the zone sensor thread did not start: %s\n",
                strerror(rc));
        return;
    }
    g_plan_started = 1;
}

static void print_person_plan(void)
{
    int done = __atomic_load_n(&g_plan_done, __ATOMIC_ACQUIRE);
    char line[256];

    if (done == 0) {
        return;
    }
    if (done < 0) {
        printf("source: person-plan failed: %s; approach_start_ns %lld\n",
               g_plan_error, (long long)g_plan_start_ns);
        return;
    }
    if (qt_line_person_plan(line, sizeof line, g_plan_start_ns,
                            g_plan_start_eye_um, g_plan_offset_ns,
                            g_plan_event_ns, g_plan_woke_ns, ARM_PLAN_MARGIN_M,
                            arm_plan_event_distance_m()) > 0) {
        fputs(line, stdout);
    }
}

/* ---- /dev/signal-source, copied from the Nerve source ---- */

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    char text[512];
    int status;
    int n;
    size_t offset;
    size_t nbytes;

    status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    if (msg->i.nbytes == 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
    ctl_lock();
    n = source_ctl_status(&g_ctl, text, sizeof text);
    ctl_unlock();
    if (n < 0) {
        return EIO;
    }
    offset = ocb->offset < 0 ? 0 : (size_t)ocb->offset;
    if (offset >= (size_t)n) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
    nbytes = (size_t)n - offset;
    if (nbytes > msg->i.nbytes) {
        nbytes = msg->i.nbytes;
    }
    if (MsgReply(ctp->rcvid, (long)nbytes, text + offset, nbytes) == -1) {
        fprintf(stderr, "source: replying to a read of %s failed: %s\n",
                SOURCE_DEV_PATH, strerror(errno));
        return _RESMGR_NOREPLY;
    }
    ocb->offset += (off_t)nbytes;
    return _RESMGR_NOREPLY;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    char text[80];
    size_t nbytes;
    ssize_t got;
    int status;
    unsigned wake;
    int64_t now;

    status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    nbytes = msg->i.nbytes;
    if (nbytes == 0 || nbytes >= sizeof text) {
        ctl_lock();
        snprintf(g_ctl.error, sizeof g_ctl.error,
                 nbytes == 0 ? "unknown command" : "command too long");
        ctl_unlock();
        return EINVAL;
    }
    got = resmgr_msgread(ctp, text, nbytes, sizeof msg->i);
    if (got < 0 || (size_t)got != nbytes) {
        return errno != 0 ? errno : EIO;
    }
    text[nbytes] = '\0';

    /* The event time of "person in" is when the sensor said so. */
    now = monotonic_ns();
    ctl_lock();
    g_ctl.wake = 0;
    status = source_ctl_apply_at(&g_ctl, text, now);
    wake = g_ctl.wake;
    g_ctl.wake = 0;
    ctl_unlock();
    if (status != 0) {
        return EINVAL;
    }
    if ((wake & SOURCE_WAKE_URGENT) != 0) {
        pulse_class(SIGNAL_CLASS_URGENT, PULSE_ZONE);
    }
    if ((wake & SOURCE_WAKE_COMMAND) != 0) {
        pulse_class(SIGNAL_CLASS_COMMAND, PULSE_ARM);
    }
    if ((wake & SOURCE_WAKE_PLAN) != 0) {
        if (!g_plan_started || sem_post(&g_plan_sem) == -1) {
            ctl_lock();
            snprintf(g_ctl.error, sizeof g_ctl.error,
                     "the zone sensor is not running");
            ctl_unlock();
            return EIO;
        }
    }
    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return EOK;
}

static void *device_thread(void *arg)
{
    dispatch_context_t *ctp = arg;

    (void)pin_cpu0();
    /* Commands at COMMAND priority on a fixed-priority channel (D-060). */
    if (set_fifo(SIGNAL_PRIO_COMMAND) != 0) {
        return NULL;
    }
    for (;;) {
        ctp = dispatch_block(ctp);
        if (ctp == NULL) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "source: waiting on %s failed: %s\n",
                    SOURCE_DEV_PATH, strerror(errno));
            return NULL;
        }
        dispatch_handler(ctp);
    }
}

static int start_device(void)
{
    static iofunc_attr_t dev_attr;
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t io_funcs;
    resmgr_attr_t resmgr_attr;
    dispatch_context_t *ctp;
    pthread_t thread;
    int attempt;
    int created;
    int id = -1;
    int chid;

    chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK |
                         _NTO_CHF_DISCONNECT);
    if (chid == -1) {
        fprintf(stderr, "source: creating the %s channel failed: %s\n",
                SOURCE_DEV_PATH, strerror(errno));
        return -1;
    }
    g_dpp = dispatch_create_channel(chid, DISPATCH_FLAG_NOLOCK);
    if (g_dpp == NULL) {
        fprintf(stderr, "source: creating the dispatch failed: %s\n",
                strerror(errno));
        return -1;
    }
    memset(&resmgr_attr, 0, sizeof resmgr_attr);
    resmgr_attr.nparts_max = 1;
    resmgr_attr.msg_max_size = 2048;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;
    io_funcs.write = io_write;
    iofunc_attr_init(&dev_attr, S_IFNAM | 0666, 0, 0);
    for (attempt = 0; attempt < FIND_ATTEMPTS; attempt++) {
        id = resmgr_attach(g_dpp, &resmgr_attr, SOURCE_DEV_PATH, _FTYPE_ANY, 0,
                           &connect_funcs, &io_funcs, &dev_attr);
        if (id != -1) {
            break;
        }
        if (errno != EBUSY && errno != EEXIST) {
            break;
        }
        sleep_until(monotonic_ns() + FIND_PAUSE_NS);
    }
    if (id == -1) {
        fprintf(stderr, "source: attaching %s failed: %s\n",
                SOURCE_DEV_PATH, strerror(errno));
        return -1;
    }
    ctp = dispatch_context_alloc(g_dpp);
    if (ctp == NULL) {
        fprintf(stderr, "source: allocating the dispatch context failed: %s\n",
                strerror(errno));
        return -1;
    }
    created = pthread_create(&thread, NULL, device_thread, ctp);
    if (created != 0) {
        fprintf(stderr, "source: starting the %s thread failed: %s\n",
                SOURCE_DEV_PATH, strerror(created));
        return -1;
    }
    printf("source: path %s\n", SOURCE_DEV_PATH);
    fflush(stdout);
    return 0;
}

/* ---- the end of the run ---- */

/*
 * Each class thread gets a quit pulse. A thread whose send the next node
 * already took waits in REPLY for that node: on a name_attach channel the
 * time limit only asks the node to unblock it (D-098, note F-028). The
 * run does not wait for it longer than JOIN_LIMIT_NS; it is named.
 */
static int end_threads(void)
{
    int64_t until = monotonic_ns() + JOIN_LIMIT_NS;
    int stuck = 0;
    unsigned i;

    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    for (i = 0; i < 3; i++) {
        struct itimerspec off;

        memset(&off, 0, sizeof off);
        if (g_class[i].timer_made) {
            (void)timer_settime(g_class[i].timer, 0, &off, NULL);
        }
        if (g_class[i].started) {
            pulse_class(g_class[i].id, PULSE_QUIT);
        }
    }
    for (i = 0; i < 3; i++) {
        if (!g_class[i].started) {
            continue;
        }
        while (!__atomic_load_n(&g_class[i].done, __ATOMIC_ACQUIRE) &&
               monotonic_ns() < until) {
            sleep_until(monotonic_ns() + 10000000LL);
        }
        if (!__atomic_load_n(&g_class[i].done, __ATOMIC_ACQUIRE)) {
            printf("source: the %s thread is still in a send after %lld ms; "
                   "the run ends without it\n", signal_class_name(g_class[i].id),
                   (long long)(JOIN_LIMIT_NS / 1000000LL));
            stuck = 1;
        } else {
            (void)pthread_join(g_class[i].thread, NULL);
        }
    }
    fflush(stdout);
    return stuck;
}

static int send_end(void)
{
    struct qt_end end;
    uint64_t limit = END_TIMEOUT_NS;
    int coid = open_next(g_next_name);
    long sent;

    if (coid == -1) {
        return -1;
    }
    memset(&end, 0, sizeof end);
    end.type = QT_MSG_END;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &limit, NULL) == -1) {
        fprintf(stderr, "source: arming the end time limit failed: %s\n",
                strerror(errno));
        (void)name_close(coid);
        return -1;
    }
    sent = MsgSend(coid, &end, sizeof end, NULL, 0);
    if (sent == -1) {
        fprintf(stderr, "source: sending the end of the run failed: %s\n",
                strerror(errno));
        (void)name_close(coid);
        return -1;
    }
    (void)name_close(coid);
    printf("source: sent the end of the run\n");
    return 0;
}

static int redirect(const char *path)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);

    if (fd == -1) {
        fprintf(stderr, "source: opening %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: source [-o FILE] [-q] live <next-name>\n");
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    struct qt_opts opts;
    int first;
    int ceiling;
    int failed = 0;
    unsigned i;
    pthread_attr_t attr;
    static const struct {
        uint32_t id;
        int prio;
        int64_t tick_ns;
    } plan[3] = {
        { SIGNAL_CLASS_URGENT, SIGNAL_PRIO_URGENT, SOURCE_URGENT_PERIOD_NS },
        { SIGNAL_CLASS_COMMAND, SIGNAL_PRIO_COMMAND, SOURCE_COMMAND_PERIOD_NS },
        { SIGNAL_CLASS_BACKGROUND, SIGNAL_PRIO_BACKGROUND, BACKGROUND_TICK_NS },
    };

    first = qt_parse_opts(argc, argv, &opts);
    if (first < 0 || qt_parse_source(argc, argv, first, &g_next_name) != 0) {
        return usage();
    }
    if (opts.out != NULL && redirect(opts.out) != 0) {
        return EXIT_FAILURE;
    }
    if (opts.quiet) {
        setenv("SIGNAL_QUIET", "1", 1);
    }
    source_ctl_init(&g_ctl, 1);
    if (pin_cpu0() != 0) {
        return EXIT_FAILURE;
    }
    ceiling = sched_get_priority_max(SCHED_FIFO);
    if (ceiling < SIGNAL_PRIO_URGENT) {
        fprintf(stderr,
                "source: SCHED_FIFO ceiling %d is below the URGENT priority %d\n",
                ceiling, SIGNAL_PRIO_URGENT);
        return EXIT_FAILURE;
    }
    printf("source: SCHED_FIFO ceiling %d, live signals to %s, build %s\n",
           ceiling, g_next_name, QT_BUILD_NAME);
    printf("source: pid %d start_ns %lld\n", (int)getpid(),
           (long long)monotonic_ns());
    fflush(stdout);
    if (start_device() != 0) {
        return EXIT_FAILURE;
    }
    start_plan();

    for (i = 0; i < 3; i++) {
        if (make_class(&g_class[i], plan[i].id, plan[i].prio,
                       plan[i].tick_ns) != 0) {
            return EXIT_FAILURE;
        }
    }
    /* Each class thread sets its own SCHED_FIFO priority as it starts. */
    pthread_attr_init(&attr);
    for (i = 0; i < 3; i++) {
        int rc = pthread_create(&g_class[i].thread, &attr, class_main,
                                &g_class[i]);

        if (rc != 0) {
            fprintf(stderr, "source: the %s thread did not start: %s\n",
                    signal_class_name(g_class[i].id), strerror(rc));
            return EXIT_FAILURE;
        }
        g_class[i].started = 1;
    }
    pthread_attr_destroy(&attr);
    for (i = 0; i < 3; i++) {
        if (start_timer(&g_class[i], g_class[i].tick_ns) != 0) {
            return EXIT_FAILURE;
        }
    }
    printf("source: threads URGENT %d every %lld ns, COMMAND %d every %lld ns, "
           "BACKGROUND %d every %lld ns (tick %lld ns); send limits %lld, "
           "%lld, %lld ns\n",
           SIGNAL_PRIO_URGENT, (long long)SOURCE_URGENT_PERIOD_NS,
           SIGNAL_PRIO_COMMAND, (long long)SOURCE_COMMAND_PERIOD_NS,
           SIGNAL_PRIO_BACKGROUND, (long long)SOURCE_BACKGROUND_PERIOD_NS,
           (long long)BACKGROUND_TICK_NS,
           (long long)qt_send_limit_ns(SIGNAL_CLASS_URGENT),
           (long long)qt_send_limit_ns(SIGNAL_CLASS_COMMAND),
           (long long)qt_send_limit_ns(SIGNAL_CLASS_BACKGROUND));
    printf("source: flood every %lld ns, batch %u, one MsgSend per signal\n",
           (long long)SOURCE_FLOOD_PERIOD_NS, SOURCE_FLOOD_BATCH);
    fflush(stdout);

    for (;;) {
        int stop;

        sleep_until(monotonic_ns() + 10000000LL);
        ctl_lock();
        stop = g_ctl.stop;
        ctl_unlock();
        if (stop) {
            break;
        }
    }

    if (end_threads() != 0) {
        /* A thread still waits in REPLY: a property of the network, not a failure. */
    }
    if (send_end() != 0) {
        failed = 1;
    }
    printf("source: sent %llu signals\n",
           (unsigned long long)__atomic_load_n(&g_next_number, __ATOMIC_ACQUIRE));
    ctl_lock();
    printf("source: issued URGENT %llu COMMAND %llu BACKGROUND %llu\n",
           (unsigned long long)g_ctl.issued_urgent,
           (unsigned long long)g_ctl.issued_command,
           (unsigned long long)g_ctl.issued_background);
    ctl_unlock();
    for (i = 0; i < 3; i++) {
        printf("source: class %s send-timeout %lu send-failed %lu\n",
               signal_class_name(g_class[i].id), g_class[i].timeouts,
               g_class[i].failures);
    }
    pthread_mutex_lock(&g_list_lock);
    print_person_urgent();
    print_person_plan();
    print_urgent_issued();
    pthread_mutex_unlock(&g_list_lock);
    printf("source: exit status %d\n", failed ? 1 : 0);
    fflush(stdout);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
