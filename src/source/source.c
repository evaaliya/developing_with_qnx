#define _QNX_SOURCE

#include "close_list.h"
#include "issue_order.h"
#include "issued_log.h"
#include "live_book.h"
#include "node_store.h"
#include "signal_msg.h"
#include "signal_queue.h"
#include "signal_sched.h"
#include "source_ctl.h"
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

#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>

_Static_assert(SIGNAL_MSG_TYPE > _IO_MAX,
               "SIGNAL_MSG_TYPE is inside the reserved system message range");
_Static_assert(SOURCE_FLOOD_BATCH == SIGNAL_BATCH_MAX,
               "the live flood batch must match the wire batch");
_Static_assert(SOURCE_FLOOD_BATCH > SIGNAL_QUEUE_CAP,
               "one flood batch must overflow the ordinary relay queue");

/* The device thread, the timer, and the flood thread share the flags. */
static struct source_control g_ctl;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_issue_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_t *g_dpp;

/*
 * Pulses to the timer thread. A zone change and an arm command are
 * issued at once, not on the next schedule time (D-080). The zone pulse
 * comes at URGENT priority, the arm pulse at COMMAND priority.
 */
#define PULSE_TICK _PULSE_CODE_MINAVAIL
#define PULSE_ZONE (_PULSE_CODE_MINAVAIL + 1)
#define PULSE_ARM (_PULSE_CODE_MINAVAIL + 2)
static int g_timer_coid = -1;

static void ctl_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void ctl_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static int telemetry_on;
/* Telemetry reads these; the live book sets them (D-063). */
static unsigned g_books_resumes;
/*
 * The live book for the mode word. Set right after the book is opened,
 * before /dev/signal-source takes commands; g_live is cleared later
 * (D-065). NULL in counted mode.
 */
static struct live_book *g_mode_book;
static int g_books_reset;
static unsigned live_waiting(void);

static void publish_source(int flush)
{
    struct telem_books books;
    char buf[TELEM_REPORT_CAP];
    enum telem_mode mode;
    uint64_t urgent;
    uint64_t command;
    uint64_t background;
    int n;

    if (!telemetry_on) {
        return;
    }
    ctl_lock();
    mode = telem_mode_from_flags(g_ctl.flood, g_ctl.urgent_on);
    urgent = g_ctl.issued_urgent;
    command = g_ctl.issued_command;
    background = g_ctl.issued_background;
    ctl_unlock();
    memset(&books, 0, sizeof books);
    books.source_resumes = g_books_resumes;
    books.source_reset = (uint32_t)g_books_reset;
    books.waiting = live_waiting();
    n = telem_report_source(buf, sizeof buf, mode, urgent, command, background,
                            &books);
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

static void open_telemetry(void)
{
    telemetry_on = telem_pub_open("source");
    if (telemetry_on) {
        publish_source(0);
    }
}

/* Class deadlines are the measured values in signal_msg.h (D-058). */
#define FIND_ATTEMPTS 40
#define FIND_PAUSE_NS 50000000L
#define SEND_TIMEOUT_SECONDS 5u
#define MANIFEST_TIMEOUT_SECONDS 20u

/*
 * Limit the next MsgSend on this thread only. alarm() is aimed at the
 * process, so with the timer, flood, and device threads its SIGALRM
 * could interrupt a thread other than the one that is stuck (D-061).
 * TimerTimeout is cleared by the next kernel call, so arm it last.
 */
static int arm_send_timeout(unsigned seconds)
{
    uint64_t timeout = (uint64_t)seconds * 1000000000ULL;

    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL) == -1) {
        fprintf(stderr, "source: arming a send timeout failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
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
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec > 999999999L ||
        ts.tv_sec > (INT64_MAX / 1000000000LL)) {
        fprintf(stderr,
                "source: the monotonic clock value does not fit in a signed "
                "64-bit nanosecond count\n");
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int open_next(const char *name)
{
    int attempt;

    for (attempt = 0; attempt < FIND_ATTEMPTS; attempt++) {
        int coid = name_open(name, 0);
        struct timespec pause;

        if (coid != -1) {
            return coid;
        }
        if (errno != ENOENT) {
            fprintf(stderr, "source: opening the next name %s failed: %s\n",
                    name, strerror(errno));
            return -1;
        }
        pause.tv_sec = 0;
        pause.tv_nsec = FIND_PAUSE_NS;
        nanosleep(&pause, NULL);
    }

    fprintf(stderr,
            "source: could not find a node registered as %s after 2 seconds\n",
            name);
    return -1;
}

static int parse_count(const char *text, unsigned long *out)
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
    *out = value;
    return 0;
}

static void pause_gap(unsigned long gap_ms)
{
    struct timespec pause;

    if (gap_ms == 0) {
        return;
    }
    pause.tv_sec = (time_t)(gap_ms / 1000UL);
    pause.tv_nsec = (long)(gap_ms % 1000UL) * 1000000L;
    nanosleep(&pause, NULL);
}

static int load_issued(struct issued_item *items, unsigned *count,
                       uint64_t *max_number)
{
    FILE *file;
    char buf[8192];
    size_t n;

    file = fopen(ISSUED_LIST_PATH, "r");
    if (file == NULL) {
        fprintf(stderr, "source: opening %s failed: %s\n",
                ISSUED_LIST_PATH, strerror(errno));
        return -1;
    }
    n = fread(buf, 1, sizeof buf - 1, file);
    if (ferror(file)) {
        fprintf(stderr, "source: reading %s failed: %s\n",
                ISSUED_LIST_PATH, strerror(errno));
        fclose(file);
        return -1;
    }
    if (!feof(file)) {
        fprintf(stderr, "source: %s is too large\n", ISSUED_LIST_PATH);
        fclose(file);
        return -1;
    }
    fclose(file);
    buf[n] = '\0';
    if (issued_list_parse(buf, items, SIGNAL_MANIFEST_CAP, count,
                          max_number) != 0) {
        fprintf(stderr, "source: %s is not a list of sent signals\n",
                ISSUED_LIST_PATH);
        return -1;
    }
    return 0;
}

static int send_one(int coid, uint64_t number, uint32_t class_id,
                    const struct signal_arm *arm);

static int send_recorded(int coid, struct signal_manifest *manifest,
                         unsigned long *recorded, uint64_t *number,
                         uint32_t class_id, unsigned long gap_ms)
{
    if (*number > 1) {
        pause_gap(gap_ms);
    }
    if (*recorded >= SIGNAL_MANIFEST_CAP ||
        send_one(coid, *number, class_id, NULL) != 0) {
        return -1;
    }
    ctl_lock();
    source_ctl_note_issued(&g_ctl, class_id);
    ctl_unlock();
    publish_source(0);
    manifest->item[*recorded].number = *number;
    manifest->item[*recorded].class_id = class_id;
    (*recorded)++;
    (*number)++;
    return 0;
}

static int send_one(int coid, uint64_t number, uint32_t class_id,
                    const struct signal_arm *arm)
{
    struct signal_message message;
    struct signal_reply reply;
    int priority;
    long sent;

    priority = signal_class_priority(class_id);
    if (priority < 0) {
        fprintf(stderr, "source: class %u has no priority\n", class_id);
        return -1;
    }
    if (set_fifo(priority) != 0) {
        return -1;
    }

    memset(&message, 0, sizeof message);
    message.type = SIGNAL_MSG_TYPE;
    message.body.number = number;
    message.body.class_id = class_id;
    message.body.issued_ns = monotonic_ns();
    message.body.deadline_ns = signal_class_deadline_ns(class_id);
    if (arm != NULL) {
        message.body.arm = *arm;
    }
    if (message.body.deadline_ns < 0) {
        fprintf(stderr, "source: class %u has no deadline\n", class_id);
        return -1;
    }

    memset(&reply, 0, sizeof reply);
    if (arm_send_timeout(SEND_TIMEOUT_SECONDS) != 0) {
        return -1;
    }
    sent = MsgSend(coid, &message, sizeof message, &reply, sizeof reply);
    if (sent == -1) {
        int err = errno;

        /*
         * The next node died (ESRCH) or its connection is gone (EBADF).
         * The live source keeps the number on its book and waits for the
         * node to come back (D-063). The counted burst stops.
         */
        if (err == ESRCH || err == EBADF) {
            if (!g_ctl.live) {
                fprintf(stderr, "source: sending signal %" PRIu64
                        " failed: %s\n", number, strerror(err));
            }
            errno = err;
            return 1;
        }
        fprintf(stderr, "source: sending signal %" PRIu64 " failed: %s\n",
                number, strerror(err));
        return -1;
    }
    if (sent != EOK) {
        fprintf(stderr, "source: the next node rejected signal %" PRIu64
                " with status %ld\n",
                number, sent);
        return -1;
    }

    /* A counted burst always prints: a restart rebuilds its list from it. */
    if (!(g_ctl.live && signal_log_quiet())) {
        printf("source: sent signal %" PRIu64 " class %s\n",
               number, signal_class_name(class_id));
        fflush(stdout);
    }
    return 0;
}

static int send_manifest(int coid, struct signal_manifest *manifest,
                         unsigned long recorded)
{
    long sent;

    /*
     * Drop below every class so the relay forward thread, still at its
     * own priority until it dequeues, can run and drain the queue.
     * The manifest is not a signal: it is forwarded before the previous
     * node is answered, so a death in the middle cannot swallow the list.
     */
    if (set_fifo(SIGNAL_PRIO_OWN) != 0) {
        return -1;
    }
    publish_source(1);
    manifest->count = (uint16_t)recorded;
    if (arm_send_timeout(MANIFEST_TIMEOUT_SECONDS) != 0) {
        return -1;
    }
    sent = MsgSend(coid, manifest, sizeof *manifest, NULL, 0);
    if (sent == -1) {
        fprintf(stderr, "source: sending the manifest failed: %s\n",
                strerror(errno));
        return -1;
    }
    if (sent != EOK) {
        fprintf(stderr,
                "source: the next node rejected the manifest with status %ld\n",
                sent);
        return -1;
    }
    printf("source: sent manifest of %lu signals\n", recorded);
    fflush(stdout);
    return 0;
}

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

    {
        /* The event time of "person in" is when the sensor said so. */
        int64_t now = monotonic_ns();
        unsigned wake;

        ctl_lock();
        g_ctl.wake = 0;
        status = source_ctl_apply_at(&g_ctl, text, now);
        if (status == 0) {
            /* A restarted source takes this mode back (D-065). */
            live_book_set_mode(g_mode_book, g_ctl.flood, g_ctl.urgent_on);
        }
        wake = g_ctl.wake;
        g_ctl.wake = 0;
        ctl_unlock();
        if (status != 0) {
            return EINVAL;
        }
        if ((wake & SOURCE_WAKE_URGENT) != 0 && g_timer_coid != -1 &&
            MsgSendPulse(g_timer_coid, SIGNAL_PRIO_URGENT, PULSE_ZONE, 0) == -1) {
            fprintf(stderr, "source: waking the timer thread for the zone "
                    "failed: %s\n", strerror(errno));
        }
        if ((wake & SOURCE_WAKE_COMMAND) != 0 && g_timer_coid != -1 &&
            MsgSendPulse(g_timer_coid, SIGNAL_PRIO_COMMAND, PULSE_ARM, 0) == -1) {
            fprintf(stderr, "source: waking the timer thread for the arm "
                    "failed: %s\n", strerror(errno));
        }
    }
    publish_source(0);
    _IO_SET_WRITE_NBYTES(ctp, nbytes);
    return EOK;
}

static void *device_thread(void *arg)
{
    dispatch_context_t *ctp = arg;

    (void)pin_cpu0();
    /*
     * Commands run at COMMAND priority on a fixed-priority channel, so
     * "flood off" is not stuck behind the BACKGROUND work it caused (D-060).
     */
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

    /*
     * A message would otherwise lend the device thread the shell's
     * priority (10), below the BACKGROUND flood (12). U1: a channel
     * given to resmgr_attach needs _NTO_CHF_UNBLOCK and _NTO_CHF_DISCONNECT.
     */
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
        pause_gap(50);
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

static int usage(void)
{
    fprintf(stderr,
            "usage: source [-s seed] [-n start] <urgent-count> "
            "<command-count> <background-count> <next-name> [gap-ms]\n"
            "       source live [-r] <next-name>\n");
    return EXIT_FAILURE;
}

/* Everything under g_issue_lock. */
struct live_state {
    int coid;
    const char *next_name;
    struct live_book *book;
    int failed;
    int stop;
    /* The next node is gone; nothing is issued until it is back (D-063). */
    int link_down;
    int64_t down_since_ns;
    unsigned long not_issued;
    unsigned long flood_skipped;
};

static struct live_state g_live;

/*
 * When the timer thread's current send began (CLOCK_MONOTONIC ns), 0 when
 * it is not sending. The watch thread reads it to see a send that the
 * time limit cannot free (F-028, F-033).
 */
static int64_t g_timer_send_since_ns;
/* The first thread to end the live run sets this; the other one waits. */
static int g_ending;
static timer_t g_ticker;
static struct signal_manifest g_manifest;

static int claim_end(void)
{
    int expected = 0;

    return __atomic_compare_exchange_n(&g_ending, &expected, 1, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Numbers that wait to be closed, for telemetry. A read without the lock
   may be one behind; the next report has the next value. */
static unsigned live_waiting(void)
{
    struct live_book *book = g_live.book;

    if (book == NULL) {
        return 0;
    }
    return __atomic_load_n(&book->ring.count, __ATOMIC_RELAXED);
}

static int link_gone(int err)
{
    return err == ESRCH || err == EBADF;
}

/*
 * The caller holds g_issue_lock. Only the first thread to see the dead
 * connection marks it; a send on a connection already replaced is not
 * a new death.
 */
static void mark_link_down_locked(int coid_used, const char *what)
{
    if (g_live.link_down || coid_used != g_live.coid) {
        return;
    }
    g_live.link_down = 1;
    g_live.down_since_ns = monotonic_ns();
    g_live.not_issued = 0;
    g_live.flood_skipped = 0;
    printf("source: %s is gone (%s); numbers on the book are closed as lost "
           "if they did not arrive, and nothing is issued until it is back\n",
           g_live.next_name, what);
    fflush(stdout);
}

/* The timer thread, each tick while the next node is gone. */
static void try_reopen(void)
{
    int down;
    int coid;
    int old;
    int64_t since;
    unsigned long skipped;
    unsigned long flood;

    pthread_mutex_lock(&g_issue_lock);
    down = g_live.link_down;
    pthread_mutex_unlock(&g_issue_lock);
    if (!down) {
        return;
    }
    coid = name_open(g_live.next_name, 0);
    if (coid == -1) {
        return;
    }
    pthread_mutex_lock(&g_issue_lock);
    old = g_live.coid;
    g_live.coid = coid;
    g_live.link_down = 0;
    since = g_live.down_since_ns;
    skipped = g_live.not_issued;
    flood = g_live.flood_skipped;
    pthread_mutex_unlock(&g_issue_lock);
    if (old >= 0 && name_close(old) == -1 && errno != EBADF) {
        fprintf(stderr, "source: closing the dead connection failed: %s\n",
                strerror(errno));
    }
    printf("source: reopened %s after %lld ms; %lu scheduled signals and %lu "
           "flood batches were not issued while it was gone\n",
           g_live.next_name, (long long)((monotonic_ns() - since) / 1000000LL),
           skipped, flood);
    fflush(stdout);
}

/*
 * One live signal. 1 when sent, 2 when the next node is gone (the
 * number stays on the book if it was taken), 0 when the book is full
 * (closes have stopped coming back), -1 when the send failed. The
 * number goes on the book before the send, so a death in the send
 * leaves it to be closed (D-063).
 */
/*
 * The URGENT that carried "person in" (D-085): kept in memory by the
 * timer thread, the only thread that issues URGENT, and printed once the
 * run has stopped, so no line is written on the URGENT path.
 */
#define PERSON_URGENT_CAP 128
#define PERSON_EVENT_CAP 8
static uint64_t g_person_urgent[PERSON_URGENT_CAP];
static unsigned g_person_urgent_n;
static unsigned g_person_urgent_missed;
static uint64_t g_person_event_number[PERSON_EVENT_CAP];
static int64_t g_person_event_ns[PERSON_EVENT_CAP];
static unsigned g_person_event_n;

static void note_person_urgent(uint64_t number, const struct signal_arm *arm)
{
    if ((arm->flags & SIGNAL_ARM_PERSON_IN) == 0) {
        return;
    }
    if ((arm->flags & SIGNAL_ARM_EVENT) != 0 && g_person_event_n < PERSON_EVENT_CAP) {
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

static void print_person_urgent(void)
{
    unsigned i;

    for (i = 0; i < g_person_event_n; i++) {
        printf("source: person-in event_ns %lld URGENT %" PRIu64 "\n",
               (long long)g_person_event_ns[i], g_person_event_number[i]);
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

static int issue_live(uint32_t class_id, uint32_t extra_flags)
{
    struct signal_arm arm;
    uint64_t number;
    int coid;
    int rc;

    pthread_mutex_lock(&g_issue_lock);
    if (g_live.link_down) {
        g_live.not_issued++;
        pthread_mutex_unlock(&g_issue_lock);
        return 2;
    }
    if (issued_ring_count(&g_live.book->ring) >= SIGNAL_MANIFEST_CAP ||
        live_book_take(g_live.book, class_id, monotonic_ns(), &number) != 0) {
        pthread_mutex_unlock(&g_issue_lock);
        return 0;
    }
    ctl_lock();
    source_ctl_note_issued(&g_ctl, class_id);
    source_ctl_arm_payload(&g_ctl, class_id, &arm);
    ctl_unlock();
    arm.flags |= extra_flags;
    if (class_id == SIGNAL_CLASS_URGENT) {
        note_person_urgent(number, &arm);
    }
    coid = g_live.coid;
    /*
     * The number is on the book. Release the lock before the send, as the
     * flood does (D-060): if the next node takes this signal and never
     * answers (F-028), the flood and close threads must still reach their
     * own send time limits instead of waiting for this lock (F-033).
     */
    pthread_mutex_unlock(&g_issue_lock);
    __atomic_store_n(&g_timer_send_since_ns, monotonic_ns(), __ATOMIC_RELEASE);
    rc = send_one(coid, number, class_id, &arm);
    __atomic_store_n(&g_timer_send_since_ns, 0, __ATOMIC_RELEASE);
    if (rc == 1) {
        char what[96];

        snprintf(what, sizeof what, "signal %" PRIu64 ": %s", number,
                 strerror(errno));
        pthread_mutex_lock(&g_issue_lock);
        mark_link_down_locked(coid, what);
        pthread_mutex_unlock(&g_issue_lock);
        publish_source(0);
        return 2;
    }
    if (rc != 0) {
        return -1;
    }
    publish_source(0);
    return 1;
}

static pthread_t g_flood_thread;
static int g_flood_started;

static int send_flood_batch(unsigned count)
{
    struct signal_batch batch;
    unsigned i;
    unsigned sent_n = 0;
    unsigned room;
    long sent;
    int64_t now;
    int64_t deadline;
    int coid;

    if (count == 0 || count > SIGNAL_BATCH_MAX) {
        return -1;
    }
    deadline = signal_class_deadline_ns(SIGNAL_CLASS_BACKGROUND);
    if (deadline < 0) {
        fprintf(stderr, "source: BACKGROUND has no deadline\n");
        return -1;
    }
    if (set_fifo(SIGNAL_PRIO_BACKGROUND) != 0) {
        return -1;
    }
    memset(&batch, 0, sizeof batch);
    batch.type = SIGNAL_MSG_BATCH;
    now = monotonic_ns();
    pthread_mutex_lock(&g_issue_lock);
    if (g_live.stop) {
        pthread_mutex_unlock(&g_issue_lock);
        return 0;
    }
    if (g_live.link_down) {
        g_live.flood_skipped++;
        pthread_mutex_unlock(&g_issue_lock);
        return 0;
    }
    /*
     * The flood may fill the ring of numbers not closed yet only up to
     * the reserve. Closes empty it every 250 ms, so this is reached only
     * when closes stop coming back (D-062).
     */
    room = source_flood_room(issued_ring_count(&g_live.book->ring),
                             SIGNAL_MANIFEST_CAP, count);
    if (room == 0) {
        unsigned waiting = issued_ring_count(&g_live.book->ring);

        pthread_mutex_unlock(&g_issue_lock);
        ctl_lock();
        g_ctl.flood = 0;
        snprintf(g_ctl.error, sizeof g_ctl.error,
                 "flood stopped: %u signals wait to be closed", waiting);
        live_book_set_mode(g_mode_book, g_ctl.flood, g_ctl.urgent_on);
        ctl_unlock();
        printf("source: flood stopped, %u of %u issued numbers wait to be "
               "closed, %u kept for the live schedule (D-062)\n",
               waiting, (unsigned)SIGNAL_MANIFEST_CAP,
               (unsigned)SOURCE_FLOOD_RESERVE);
        fflush(stdout);
        publish_source(0);
        return 0;
    }
    count = room;
    for (i = 0; i < count; i++) {
        uint64_t number;

        if (live_book_take(g_live.book, SIGNAL_CLASS_BACKGROUND, now,
                           &number) != 0) {
            break;
        }
        batch.body[sent_n].number = number;
        batch.body[sent_n].class_id = SIGNAL_CLASS_BACKGROUND;
        batch.body[sent_n].issued_ns = now;
        batch.body[sent_n].deadline_ns = deadline;
        ctl_lock();
        source_ctl_note_issued(&g_ctl, SIGNAL_CLASS_BACKGROUND);
        ctl_unlock();
        sent_n++;
    }
    if (sent_n == 0) {
        pthread_mutex_unlock(&g_issue_lock);
        return 0;
    }
    batch.count = (uint16_t)sent_n;
    coid = g_live.coid;
    /*
     * The numbers are on the issued list. Release the lock before the
     * send, so the URGENT timer thread never waits for a batch to be
     * admitted (D-060). The send has its own time limit (D-061).
     */
    pthread_mutex_unlock(&g_issue_lock);
    if (arm_send_timeout(SEND_TIMEOUT_SECONDS) != 0) {
        pthread_mutex_lock(&g_issue_lock);
        g_live.failed = 1;
        pthread_mutex_unlock(&g_issue_lock);
        return -1;
    }
    sent = MsgSend(coid, &batch, sizeof batch, NULL, 0);
    if (sent == -1 && link_gone(errno)) {
        char what[96];

        snprintf(what, sizeof what, "a BACKGROUND batch: %s", strerror(errno));
        pthread_mutex_lock(&g_issue_lock);
        mark_link_down_locked(coid, what);
        pthread_mutex_unlock(&g_issue_lock);
        return 0;
    }
    if (sent == -1 || sent != EOK) {
        if (sent == -1 && errno == ETIMEDOUT) {
            fprintf(stderr, "source: the next node did not take a BACKGROUND "
                    "batch within %u s; the flood thread stops\n",
                    SEND_TIMEOUT_SECONDS);
        } else if (sent == -1) {
            fprintf(stderr, "source: sending a BACKGROUND batch failed: %s\n",
                    strerror(errno));
        } else {
            fprintf(stderr, "source: the next node rejected a BACKGROUND batch "
                    "with status %ld\n", sent);
        }
        pthread_mutex_lock(&g_issue_lock);
        g_live.failed = 1;
        pthread_mutex_unlock(&g_issue_lock);
        return -1;
    }
    if (!signal_log_quiet()) {
        for (i = 0; i < sent_n; i++) {
            printf("source: sent signal %" PRIu64 " class %s\n",
                   batch.body[i].number,
                   signal_class_name(SIGNAL_CLASS_BACKGROUND));
            printf("source: flood signal %" PRIu64 " at %lld ns\n",
                   batch.body[i].number, (long long)now);
        }
        printf("source: flood batch %u at %lld ns\n", sent_n, (long long)now);
        fflush(stdout);
    }
    publish_source(0);
    return 1;
}

static void *flood_thread(void *arg)
{
    int64_t next_ns = 0;
    struct timespec pause;

    (void)arg;
    if (pin_cpu0() != 0) {
        pthread_mutex_lock(&g_issue_lock);
        g_live.failed = 1;
        pthread_mutex_unlock(&g_issue_lock);
        return NULL;
    }
    memset(&pause, 0, sizeof pause);
    pause.tv_nsec = 10000000L;
    for (;;) {
        int flood;
        int stop;
        unsigned batch;

        nanosleep(&pause, NULL);
        pthread_mutex_lock(&g_issue_lock);
        stop = g_live.stop || g_live.failed;
        pthread_mutex_unlock(&g_issue_lock);
        if (stop) {
            break;
        }
        ctl_lock();
        flood = g_ctl.flood;
        stop = g_ctl.stop;
        ctl_unlock();
        if (stop) {
            break;
        }
        if (!flood) {
            next_ns = 0;
            continue;
        }
        batch = source_flood_due(1, monotonic_ns(), &next_ns);
        if (batch == 0) {
            continue;
        }
        if (send_flood_batch(batch) < 0) {
            break;
        }
    }
    return NULL;
}

/*
 * The send time limit frees a batch that waits to be received. A batch
 * the relay already received waits for its reply, and on a name_attach
 * channel (_NTO_CHF_UNBLOCK) the limit only asks the relay to unblock
 * the sender. A stopped relay never answers, so the run does not wait
 * for this thread forever; exit ends it (D-061).
 */
static int join_thread(pthread_t thread, const char *what)
{
    struct timespec until;
    int rc;

    if (clock_gettime(CLOCK_MONOTONIC, &until) != 0) {
        fprintf(stderr, "source: reading the monotonic clock failed: %s\n",
                strerror(errno));
        return -1;
    }
    until.tv_sec += (time_t)SEND_TIMEOUT_SECONDS + 1;
    rc = pthread_timedjoin_monotonic(thread, NULL, &until);
    if (rc == ETIMEDOUT) {
        fprintf(stderr, "source: the %s thread is still waiting for the "
                "next node after %u s; ending the run without it\n",
                what, SEND_TIMEOUT_SECONDS + 1);
        return -1;
    }
    if (rc != EOK) {
        fprintf(stderr, "source: joining the %s thread failed: %s\n",
                what, strerror(rc));
        return -1;
    }
    return 0;
}

static int join_flood_thread(void)
{
    return join_thread(g_flood_thread, "flood");
}

static pthread_t g_close_thread;
static int g_close_started;
static pthread_t g_watch_thread;
static int g_watch_started;

/*
 * Every CLOSE_PERIOD_NS, close the numbers issued at least
 * CLOSE_SETTLE_NS ago: send them through the chain and drop them from
 * the ring once sink has them. BACKGROUND priority, so the flood does
 * not starve it and URGENT and COMMAND go first. A failed close keeps
 * the numbers and is tried again (D-062).
 */
static void *close_thread(void *arg)
{
    static struct signal_close close;
    struct timespec pause;
    int failing = 0;

    (void)arg;
    if (pin_cpu0() != 0 || set_fifo(SIGNAL_PRIO_BACKGROUND) != 0) {
        pthread_mutex_lock(&g_issue_lock);
        g_live.failed = 1;
        pthread_mutex_unlock(&g_issue_lock);
        return NULL;
    }
    memset(&pause, 0, sizeof pause);
    pause.tv_nsec = (long)CLOSE_PERIOD_NS;
    for (;;) {
        unsigned n;
        unsigned waiting;
        long sent;
        int stop;
        int down;
        int coid;

        nanosleep(&pause, NULL);
        pthread_mutex_lock(&g_issue_lock);
        stop = g_live.stop || g_live.failed;
        down = g_live.link_down;
        coid = g_live.coid;
        n = (stop || down) ? 0
                           : issued_ring_closable(&g_live.book->ring,
                                                  monotonic_ns(),
                                                  CLOSE_SETTLE_NS, close.item,
                                                  SIGNAL_CLOSE_CAP);
        pthread_mutex_unlock(&g_issue_lock);
        if (stop) {
            break;
        }
        if (n == 0) {
            continue;
        }
        close.type = SIGNAL_MSG_CLOSE;
        close.count = (uint16_t)n;
        close.reserved = 0;
        if (arm_send_timeout(SEND_TIMEOUT_SECONDS) != 0) {
            continue;
        }
        sent = MsgSend(coid, &close, sizeof close, NULL, 0);
        if (sent == -1 && link_gone(errno)) {
            char what[96];

            snprintf(what, sizeof what, "a close: %s", strerror(errno));
            pthread_mutex_lock(&g_issue_lock);
            mark_link_down_locked(coid, what);
            pthread_mutex_unlock(&g_issue_lock);
            continue;
        }
        if (sent == -1 || sent != EOK) {
            if (!failing) {
                fprintf(stderr, "source: closing signals %" PRIu64 "..%" PRIu64
                        " failed: %s; keeping them and trying again\n",
                        close.item[0].number, close.item[n - 1].number,
                        sent == -1 ? strerror(errno) : strerror((int)sent));
                failing = 1;
            }
            continue;
        }
        pthread_mutex_lock(&g_issue_lock);
        live_book_close_front(g_live.book, n);
        waiting = issued_ring_count(&g_live.book->ring);
        pthread_mutex_unlock(&g_issue_lock);
        if (failing) {
            printf("source: closes are coming back again\n");
            failing = 0;
        }
        printf("source: closed %u signals %" PRIu64 "..%" PRIu64
               ", waiting %u\n",
               n, close.item[0].number, close.item[n - 1].number, waiting);
        fflush(stdout);
    }
    return NULL;
}

/*
 * The book in shared memory (D-063). Without -r new books. With -r the
 * dead source's book, so numbers go on after its last one and its open
 * numbers are closed. If there is no whole book, a line says the
 * totals before the restart are gone. If shared memory fails, the book
 * lives in this process only, and that is said.
 */
static struct live_book *open_book(int resume)
{
    static struct live_book local;
    struct live_book_resume r;
    enum node_store_found found = NODE_STORE_NEW;
    struct live_book *book;

    book = node_store_map(LIVE_BOOK_STORE, sizeof *book, resume, &found);
    if (book == NULL) {
        fprintf(stderr, "source: shared memory %s failed: %s; the book will "
                "not outlive this process\n", LIVE_BOOK_STORE, strerror(errno));
        book = &local;
        found = resume ? NODE_STORE_MISSING : NODE_STORE_NEW;
    }
    if (found == NODE_STORE_KEPT && live_book_resume(book, &r) == 0) {
        g_ctl.issued_urgent = r.issued[SIGNAL_CLASS_URGENT];
        g_ctl.issued_command = r.issued[SIGNAL_CLASS_COMMAND];
        g_ctl.issued_background = r.issued[SIGNAL_CLASS_BACKGROUND];
        g_books_resumes = book->resumes;
        printf("source: resumed the book of a dead source (resume %u): next "
               "%" PRIu64 ", %u numbers wait to be closed, issued URGENT %"
               PRIu64 " COMMAND %" PRIu64 " BACKGROUND %" PRIu64 "%s%s\n",
               (unsigned)book->resumes, r.next, r.waiting,
               r.issued[SIGNAL_CLASS_URGENT], r.issued[SIGNAL_CLASS_COMMAND],
               r.issued[SIGNAL_CLASS_BACKGROUND],
               r.journal_redone ? "; finished a close the dead source began" : "",
               r.next_moved ? "; its last number was on the book but not sent "
                              "for sure" : "");
        {
            char line[256];

            /* The mode of the dead source, or why not (D-065). */
            (void)source_ctl_restore_mode(&g_ctl, r.mode_known, r.flood,
                                          r.urgent_on, r.waiting,
                                          SIGNAL_MANIFEST_CAP, line,
                                          sizeof line);
            live_book_set_mode(book, g_ctl.flood, g_ctl.urgent_on);
            printf("%s\n", line);
        }
        fflush(stdout);
        return book;
    }
    live_book_init(book);
    if (resume) {
        g_books_reset = 1;
        printf("source: books reset after a restart (%s); numbers start at 1 "
               "again and totals before this restart are not included\n",
               found == NODE_STORE_KEPT ? "saved book was not whole"
                                        : node_store_found_text(found));
        fflush(stdout);
    }
    return book;
}

/*
 * Ends the live run: stop the flood, close and watch threads (each given
 * up after the send time limit), stop the timer, and send the manifest
 * of everything not closed yet. Called by whichever thread claimed the
 * end: the timer thread, or the watch thread when the timer thread is
 * stuck in a send (F-033).
 */
static int end_live(int failed)
{
    struct live_book *book = g_live.book;
    const char *next_name = g_live.next_name;
    struct itimerspec spec;
    unsigned long recorded;
    int coid;

    pthread_mutex_lock(&g_issue_lock);
    g_live.stop = 1;
    if (g_live.failed) {
        failed = 1;
    }
    pthread_mutex_unlock(&g_issue_lock);
    if (g_flood_started) {
        if (join_flood_thread() != 0) {
            failed = 1;
        }
        g_flood_started = 0;
    }
    if (g_close_started) {
        if (join_thread(g_close_thread, "close") != 0) {
            failed = 1;
        }
        g_close_started = 0;
    }
    if (g_watch_started && !pthread_equal(pthread_self(), g_watch_thread)) {
        if (join_thread(g_watch_thread, "watch") != 0) {
            failed = 1;
        }
        g_watch_started = 0;
    }

    memset(&spec, 0, sizeof spec);
    if (timer_settime(g_ticker, 0, &spec, NULL) == -1) {
        fprintf(stderr, "source: stopping the timer failed: %s\n",
                strerror(errno));
        failed = 1;
    }
    /* The manifest closes whatever the close thread had not closed yet. */
    recorded = issued_ring_all(&book->ring, g_manifest.item,
                               SIGNAL_MANIFEST_CAP);
    coid = g_live.coid;
    if (g_live.link_down) {
        fprintf(stderr, "source: %s is gone at the end; the manifest of %lu "
                "signals cannot be sent and stays on the book\n",
                next_name, recorded);
        return EXIT_FAILURE;
    }
    if (send_manifest(coid, &g_manifest, recorded) != 0) {
        name_close(coid);
        return EXIT_FAILURE;
    }
    /* sink has every number now and a clean exit is not restarted. */
    live_book_close_front(book, recorded);
    if (node_store_remove(LIVE_BOOK_STORE) != 0) {
        fprintf(stderr, "source: removing %s failed: %s\n", LIVE_BOOK_STORE,
                strerror(errno));
    }
    if (name_close(coid) == -1) {
        fprintf(stderr, "source: closing the connection to %s failed: %s\n",
                next_name, strerror(errno));
        return EXIT_FAILURE;
    }
    printf("source: sent %llu signals\n", (unsigned long long)(book->next - 1));
    ctl_lock();
    printf("source: issued URGENT %llu COMMAND %llu BACKGROUND %llu\n",
           (unsigned long long)g_ctl.issued_urgent,
           (unsigned long long)g_ctl.issued_command,
           (unsigned long long)g_ctl.issued_background);
    ctl_unlock();
    print_person_urgent();
    fflush(stdout);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

/*
 * Every 250 ms: is the timer thread stuck in a send? The send time limit
 * frees a send the next node has not taken. A send it took waits for the
 * reply, and on a name_attach channel the limit only asks the node to
 * unblock it; a stopped node never does (F-028). The timer thread is the
 * one that ends the run, so without this the run would never end and
 * the flood and close threads would never be given up (F-033). COMMAND
 * priority, so the flood does not hold it off.
 */
static void *watch_thread(void *arg)
{
    struct timespec pause;
    const int64_t limit = ((int64_t)SEND_TIMEOUT_SECONDS + 1) * 1000000000LL;

    (void)arg;
    if (pin_cpu0() != 0 || set_fifo(SIGNAL_PRIO_COMMAND) != 0) {
        pthread_mutex_lock(&g_issue_lock);
        g_live.failed = 1;
        pthread_mutex_unlock(&g_issue_lock);
        return NULL;
    }
    memset(&pause, 0, sizeof pause);
    pause.tv_nsec = (long)CLOSE_PERIOD_NS;
    for (;;) {
        int64_t since;
        int64_t now;
        int stop;

        nanosleep(&pause, NULL);
        pthread_mutex_lock(&g_issue_lock);
        stop = g_live.stop;
        pthread_mutex_unlock(&g_issue_lock);
        if (stop) {
            break;
        }
        since = __atomic_load_n(&g_timer_send_since_ns, __ATOMIC_ACQUIRE);
        now = monotonic_ns();
        if (!source_send_stuck(since, now, limit)) {
            continue;
        }
        if (!claim_end()) {
            break;
        }
        fprintf(stderr, "source: a signal sent %lld ms ago has no reply: %s "
                "took it and does not answer, and the send time limit of %u "
                "s cannot free it; the timer thread is stuck, the run ends "
                "without it\n",
                (long long)((now - since) / 1000000LL), g_live.next_name,
                SEND_TIMEOUT_SECONDS);
        pthread_mutex_lock(&g_issue_lock);
        g_live.failed = 1;
        pthread_mutex_unlock(&g_issue_lock);
        (void)end_live(1);
        fflush(stdout);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    return NULL;
}

static int run_live(int argc, char **argv)
{
    struct live_book *book;
    struct sigevent event;
    struct itimerspec spec;
    int64_t next_ns[3];
    int resume = 0;
    int coid;
    int timer_chid;
    int timer_coid;
    int ceiling;
    int full = 0;
    int failed = 0;
    const char *next_name;

    if (argc == 4 && strcmp(argv[2], "-r") == 0) {
        resume = 1;
    } else if (argc != 3) {
        return usage();
    }
    next_name = argv[argc - 1];
    if (next_name[0] == '\0') {
        return usage();
    }
    source_ctl_init(&g_ctl, 1);
    book = open_book(resume);
    g_mode_book = book;
    open_telemetry();
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
           ceiling, next_name, SIGNAL_BUILD_NAME);
    fflush(stdout);
    if (start_device() != 0) {
        return EXIT_FAILURE;
    }

    coid = open_next(next_name);
    if (coid == -1) {
        return EXIT_FAILURE;
    }

    timer_chid = ChannelCreate(0);
    if (timer_chid == -1) {
        fprintf(stderr, "source: creating the timer channel failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    timer_coid = ConnectAttach(0, 0, timer_chid, _NTO_SIDE_CHANNEL, 0);
    if (timer_coid == -1) {
        fprintf(stderr, "source: attaching the timer channel failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    memset(&event, 0, sizeof event);
    /*
     * The tick arrives at URGENT priority. At the inherited own priority
     * (10) the timer thread never ran while BACKGROUND work at 12 kept
     * CPU 0 busy, and URGENT stopped being issued (D-060).
     */
    SIGEV_PULSE_INIT(&event, timer_coid, SIGNAL_PRIO_URGENT, PULSE_TICK, 0);
    if (MsgRegisterEvent(&event, timer_coid) == -1) {
        fprintf(stderr, "source: registering the timer pulse failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    if (timer_create(CLOCK_MONOTONIC, &event, &g_ticker) == -1) {
        fprintf(stderr, "source: creating the timer failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    memset(&spec, 0, sizeof spec);
    spec.it_value.tv_nsec = SOURCE_TICK_NS;
    spec.it_interval.tv_nsec = SOURCE_TICK_NS;
    if (timer_settime(g_ticker, 0, &spec, NULL) == -1) {
        fprintf(stderr, "source: starting the timer failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    g_timer_coid = timer_coid;
    printf("source: timer pulse every %lld ns, URGENT %lld ns, "
           "COMMAND %lld ns, BACKGROUND %lld ns\n",
           (long long)SOURCE_TICK_NS,
           (long long)SOURCE_URGENT_PERIOD_NS,
           (long long)SOURCE_COMMAND_PERIOD_NS,
           (long long)SOURCE_BACKGROUND_PERIOD_NS);
    printf("source: flood every %lld ns, first %u, burst %u, cap %u\n",
           (long long)SOURCE_FLOOD_PERIOD_NS, SOURCE_FLOOD_BATCH,
           SOURCE_FLOOD_BATCH, SOURCE_FLOOD_BATCH);
    fflush(stdout);

    memset(&g_manifest, 0, sizeof g_manifest);
    g_manifest.type = SIGNAL_MSG_MANIFEST;
    next_ns[0] = 0;
    next_ns[1] = 0;
    next_ns[2] = 0;
    memset(&g_live, 0, sizeof g_live);
    g_live.coid = coid;
    g_live.next_name = next_name;
    g_live.book = book;
    if (pthread_create(&g_flood_thread, NULL, flood_thread, NULL) != 0) {
        fprintf(stderr, "source: creating the flood thread failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    g_flood_started = 1;
    if (pthread_create(&g_close_thread, NULL, close_thread, NULL) != 0) {
        fprintf(stderr, "source: creating the close thread failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    g_close_started = 1;
    if (pthread_create(&g_watch_thread, NULL, watch_thread, NULL) != 0) {
        fprintf(stderr, "source: creating the watch thread failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    g_watch_started = 1;

    for (;;) {
        struct _pulse pulse;
        rcvid_t rcvid;
        int stop;

        ctl_lock();
        stop = g_ctl.stop;
        ctl_unlock();
        if (stop) {
            break;
        }
        pthread_mutex_lock(&g_issue_lock);
        failed = g_live.failed;
        pthread_mutex_unlock(&g_issue_lock);
        if (failed) {
            break;
        }
        /*
         * Wait for the tick at URGENT priority, not at the own priority.
         * A pulse raises only a thread already blocked in MsgReceive. At
         * 10 this thread stayed READY on CPU 0 behind the BACKGROUND
         * flood work at 12 for up to 3.5 s before it reached MsgReceive,
         * and S2 issued 55-75 URGENT in its 5 s window instead of ~215
         * (D-068). Blocked in MsgReceive it uses no CPU.
         */
        if (set_fifo(SIGNAL_PRIO_URGENT) != 0) {
            failed = 1;
            break;
        }

        rcvid = MsgReceive(timer_chid, &pulse, sizeof pulse, NULL);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "source: waiting for the timer failed: %s\n",
                    strerror(errno));
            failed = 1;
            break;
        }
        if (rcvid != 0) {
            if (MsgError(rcvid, ENOSYS) == -1) {
                fprintf(stderr, "source: rejecting a timer-channel message failed: %s\n",
                        strerror(errno));
            }
            continue;
        }
        if (pulse.code != PULSE_TICK && pulse.code != PULSE_ZONE &&
            pulse.code != PULSE_ARM) {
            continue;
        }
        try_reopen();
        if (pulse.code == PULSE_ZONE || pulse.code == PULSE_ARM) {
            /*
             * "person in" or "person out": the zone URGENT goes now, not
             * on the next tick; the schedule keeps its times. An arm
             * command: its COMMAND goes now and the COMMAND schedule
             * starts again from now (D-080).
             */
            uint32_t class_id = pulse.code == PULSE_ZONE ? SIGNAL_CLASS_URGENT
                                                         : SIGNAL_CLASS_COMMAND;
            int issued = issue_live(class_id, pulse.code == PULSE_ZONE
                                                  ? SIGNAL_ARM_EVENT : 0);

            if (issued < 0) {
                failed = 1;
                break;
            }
            if (class_id == SIGNAL_CLASS_COMMAND) {
                ctl_lock();
                source_schedule_restart(SIGNAL_CLASS_COMMAND, monotonic_ns(),
                                        next_ns);
                ctl_unlock();
            }
            continue;
        }

        ctl_lock();
        stop = g_ctl.stop;
        ctl_unlock();
        if (stop) {
            break;
        }

        {
            for (;;) {
                uint32_t class_id;
                int64_t now;

                ctl_lock();
                stop = g_ctl.stop;
                ctl_unlock();
                if (stop) {
                    break;
                }
                now = monotonic_ns();
                ctl_lock();
                class_id = source_schedule_take(&g_ctl, now, next_ns);
                ctl_unlock();
                if (class_id == 0) {
                    break;
                }
                {
                    int issued = issue_live(class_id, 0);

                    if (issued < 0) {
                        failed = 1;
                        break;
                    }
                    if (issued == 2) {
                        /* The next node is gone; the schedule moves on. */
                        continue;
                    }
                    if (issued == 0) {
                        /* Closes stopped coming back; nothing can be issued. */
                        if (!full) {
                            fprintf(stderr, "source: the issued list is full; "
                                    "waiting for closes before sending more\n");
                            full = 1;
                        }
                        break;
                    }
                    if (full) {
                        printf("source: the issued list has room again\n");
                        fflush(stdout);
                        full = 0;
                    }
                }
            }
        }
        if (failed) {
            break;
        }
    }

    if (!claim_end()) {
        /*
         * The watch thread is already ending the run: the next node
         * answered the stuck send late. That thread exits the process.
         */
        for (;;) {
            pause();
        }
    }
    return end_live(failed);
}

int main(int argc, char **argv)
{
    unsigned long urgent;
    unsigned long command;
    unsigned long background;
    unsigned long gap_ms = 0;
    unsigned long total;
    unsigned long recorded = 0;
    uint64_t number = 1;
    int coid;
    int ceiling;
    int arg0 = 1;
    int have_seed = 0;
    int have_start = 0;
    uint32_t seed = 0;

    if (argc >= 2 && strcmp(argv[1], "live") == 0) {
        return run_live(argc, argv);
    }
    uint32_t plan[SIGNAL_QUEUE_CAP];
    uint32_t planned = 0;
    uint32_t step;
    static struct signal_manifest manifest;
    static struct issued_item prior[SIGNAL_MANIFEST_CAP];
    unsigned prior_count = 0;

    while (arg0 < argc && argv[arg0][0] == '-' && argv[arg0][1] != '\0') {
        unsigned long value;

        if (arg0 + 1 >= argc || parse_count(argv[arg0 + 1], &value) != 0) {
            return usage();
        }
        if (strcmp(argv[arg0], "-s") == 0) {
            if (have_seed || value > UINT32_MAX) {
                return usage();
            }
            have_seed = 1;
            seed = (uint32_t)value;
        } else if (strcmp(argv[arg0], "-n") == 0) {
            if (have_start || value < 1 || value > SIGNAL_LEDGER_CAP) {
                fprintf(stderr, "source: -n must be 1..%d\n", SIGNAL_LEDGER_CAP);
                return EXIT_FAILURE;
            }
            have_start = 1;
            number = value;
        } else {
            return usage();
        }
        arg0 += 2;
    }
    if ((argc - arg0 != 4 && argc - arg0 != 5) ||
        parse_count(argv[arg0], &urgent) != 0 ||
        parse_count(argv[arg0 + 1], &command) != 0 ||
        parse_count(argv[arg0 + 2], &background) != 0 ||
        argv[arg0 + 3][0] == '\0' ||
        (argc - arg0 == 5 && parse_count(argv[arg0 + 4], &gap_ms) != 0)) {
        return usage();
    }

    if (urgent > SIGNAL_QUEUE_CAP || command > SIGNAL_QUEUE_CAP ||
        background > SIGNAL_QUEUE_CAP) {
        fprintf(stderr,
                "source: the burst must be 1..%d signals so one relay queue "
                "can hold it\n",
                SIGNAL_QUEUE_CAP);
        return EXIT_FAILURE;
    }
    total = urgent + command + background;
    if (total == 0 || total > SIGNAL_QUEUE_CAP) {
        fprintf(stderr,
                "source: the burst must be 1..%d signals so one relay queue "
                "can hold it\n",
                SIGNAL_QUEUE_CAP);
        return EXIT_FAILURE;
    }

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
    printf("source: SCHED_FIFO ceiling %d, sending %lu URGENT, %lu COMMAND, "
           "%lu BACKGROUND to %s\n",
           ceiling, urgent, command, background, argv[arg0 + 3]);
    fflush(stdout);

    source_ctl_init(&g_ctl, 0);
    open_telemetry();
    if (start_device() != 0) {
        return EXIT_FAILURE;
    }

    coid = open_next(argv[arg0 + 3]);
    if (coid == -1) {
        return EXIT_FAILURE;
    }

    memset(&manifest, 0, sizeof manifest);
    manifest.type = SIGNAL_MSG_MANIFEST;
    if (have_start) {
        uint64_t max_number = 0;
        uint64_t expect = 0;
        unsigned manifest_total = 0;
        unsigned item;

        if (load_issued(prior, &prior_count, &max_number) != 0) {
            name_close(coid);
            return EXIT_FAILURE;
        }
        if (issued_list_restart_number(max_number, prior_count > 0, &expect) != 0 ||
            expect != number ||
            issued_list_fits(prior, prior_count, number, (unsigned)total,
                             SIGNAL_MANIFEST_CAP, &manifest_total) != 0) {
            fprintf(stderr,
                    "source: the restarted list does not continue from %s\n",
                    ISSUED_LIST_PATH);
            name_close(coid);
            return EXIT_FAILURE;
        }
        for (item = 0; item < prior_count; item++) {
            manifest.item[recorded].number = prior[item].number;
            manifest.item[recorded].class_id = prior[item].class_id;
            recorded++;
        }
        manifest.reserved = SIGNAL_MANIFEST_REBUILT;
    }
    if (issue_order_burst(have_seed, seed, (uint32_t)urgent, (uint32_t)command,
                          (uint32_t)background, plan, SIGNAL_QUEUE_CAP,
                          &planned) != 0 ||
        planned != total) {
        fprintf(stderr, "source: could not lay out the burst\n");
        name_close(coid);
        return EXIT_FAILURE;
    }
    for (step = 0; step < planned; step++) {
        if (send_recorded(coid, &manifest, &recorded, &number, plan[step],
                          gap_ms) != 0) {
            name_close(coid);
            return EXIT_FAILURE;
        }
    }

    if (send_manifest(coid, &manifest, recorded) != 0) {
        name_close(coid);
        return EXIT_FAILURE;
    }

    if (name_close(coid) == -1) {
        fprintf(stderr, "source: closing the connection to %s failed: %s\n",
                argv[arg0 + 3], strerror(errno));
        return EXIT_FAILURE;
    }

    printf("source: sent %lu signals\n", total);
    fflush(stdout);
    return EXIT_SUCCESS;
}
