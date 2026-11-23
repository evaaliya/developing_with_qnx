#define _QNX_SOURCE

#include "issue_order.h"
#include "issued_log.h"
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

static void ctl_lock(void)
{
    pthread_mutex_lock(&g_lock);
}

static void ctl_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static int telemetry_on;

static void publish_source(int flush)
{
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
    n = telem_report_source(buf, sizeof buf, mode, urgent, command, background);
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
        fprintf(stderr, "source: could not arrange a send timeout: %s\n",
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

static int send_one(int coid, uint64_t number, uint32_t class_id);

static int send_recorded(int coid, struct signal_manifest *manifest,
                         unsigned long *recorded, uint64_t *number,
                         uint32_t class_id, unsigned long gap_ms)
{
    if (*number > 1) {
        pause_gap(gap_ms);
    }
    if (*recorded >= SIGNAL_MANIFEST_CAP ||
        send_one(coid, *number, class_id) != 0) {
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

static int send_one(int coid, uint64_t number, uint32_t class_id)
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
    if (message.body.deadline_ns < 0) {
        fprintf(stderr, "source: class %u has no deadline\n", class_id);
        return -1;
    }

    memset(&reply, 0, sizeof reply);
    alarm(SEND_TIMEOUT_SECONDS);
    sent = MsgSend(coid, &message, sizeof message, &reply, sizeof reply);
    alarm(0);
    if (sent == -1) {
        fprintf(stderr, "source: sending signal %" PRIu64 " failed: %s\n",
                number, strerror(errno));
        return -1;
    }
    if (sent != EOK) {
        fprintf(stderr, "source: the next node rejected signal %" PRIu64
                " with status %ld\n",
                number, sent);
        return -1;
    }

    printf("source: sent signal %" PRIu64 " class %s\n",
           number, signal_class_name(class_id));
    fflush(stdout);
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
    alarm(MANIFEST_TIMEOUT_SECONDS);
    sent = MsgSend(coid, manifest, sizeof *manifest, NULL, 0);
    alarm(0);
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

    ctl_lock();
    status = source_ctl_apply(&g_ctl, text);
    ctl_unlock();
    if (status != 0) {
        return EINVAL;
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
            "       source live <next-name>\n");
    return EXIT_FAILURE;
}

static int issue_one(int coid, struct signal_manifest *manifest,
                     unsigned long *recorded, uint64_t *number,
                     uint32_t class_id, int *full)
{
    int rc;

    if (*full || *recorded >= SIGNAL_MANIFEST_CAP) {
        if (!*full) {
            fprintf(stderr, "source: the issued list is full; not sending more\n");
            *full = 1;
        }
        return 0;
    }
    pthread_mutex_lock(&g_issue_lock);
    if (*full || *recorded >= SIGNAL_MANIFEST_CAP) {
        pthread_mutex_unlock(&g_issue_lock);
        if (!*full) {
            fprintf(stderr, "source: the issued list is full; not sending more\n");
            *full = 1;
        }
        return 0;
    }
    rc = send_recorded(coid, manifest, recorded, number, class_id, 0);
    pthread_mutex_unlock(&g_issue_lock);
    if (rc != 0) {
        return -1;
    }
    return 1;
}

struct live_book {
    int coid;
    struct signal_manifest *manifest;
    unsigned long *recorded;
    uint64_t *number;
    int *full;
    int failed;
    int stop;
};

static struct live_book g_live;
static pthread_t g_flood_thread;
static int g_flood_started;

static int send_flood_batch(unsigned count)
{
    struct signal_batch batch;
    unsigned i;
    unsigned sent_n = 0;
    long sent;
    int64_t now;
    int64_t deadline;

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
    for (i = 0; i < count; i++) {
        if (*g_live.full || *g_live.recorded >= SIGNAL_MANIFEST_CAP) {
            if (!*g_live.full) {
                fprintf(stderr,
                        "source: the issued list is full; not sending more\n");
                *g_live.full = 1;
            }
            break;
        }
        batch.body[sent_n].number = *g_live.number;
        batch.body[sent_n].class_id = SIGNAL_CLASS_BACKGROUND;
        batch.body[sent_n].issued_ns = now;
        batch.body[sent_n].deadline_ns = deadline;
        g_live.manifest->item[*g_live.recorded].number = *g_live.number;
        g_live.manifest->item[*g_live.recorded].class_id = SIGNAL_CLASS_BACKGROUND;
        ctl_lock();
        source_ctl_note_issued(&g_ctl, SIGNAL_CLASS_BACKGROUND);
        ctl_unlock();
        (*g_live.recorded)++;
        (*g_live.number)++;
        sent_n++;
    }
    if (sent_n == 0) {
        pthread_mutex_unlock(&g_issue_lock);
        return 0;
    }
    batch.count = (uint16_t)sent_n;
    /*
     * The numbers are on the issued list. Release the lock before the
     * send, so the URGENT timer thread never waits for a batch to be
     * admitted (D-060).
     */
    pthread_mutex_unlock(&g_issue_lock);
    sent = MsgSend(g_live.coid, &batch, sizeof batch, NULL, 0);
    if (sent == -1 || sent != EOK) {
        if (sent == -1) {
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
    for (i = 0; i < sent_n; i++) {
        printf("source: sent signal %" PRIu64 " class %s\n",
               batch.body[i].number, signal_class_name(SIGNAL_CLASS_BACKGROUND));
        printf("source: flood signal %" PRIu64 " at %lld ns\n",
               batch.body[i].number, (long long)now);
    }
    printf("source: flood batch %u at %lld ns\n", sent_n, (long long)now);
    fflush(stdout);
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

static int run_live(int argc, char **argv)
{
    static struct signal_manifest manifest;
    struct sigevent event;
    struct itimerspec spec;
    int64_t next_ns[3];
    unsigned long recorded = 0;
    uint64_t number = 1;
    int coid;
    int timer_chid;
    int timer_coid;
    timer_t ticker;
    int ceiling;
    int full = 0;
    int failed = 0;
    const char *next_name;

    if (argc != 3 || argv[2][0] == '\0') {
        return usage();
    }
    next_name = argv[2];
    source_ctl_init(&g_ctl, 1);
    open_telemetry();
    if (install_alarm() != 0 || pin_cpu0() != 0) {
        return EXIT_FAILURE;
    }
    ceiling = sched_get_priority_max(SCHED_FIFO);
    if (ceiling < SIGNAL_PRIO_URGENT) {
        fprintf(stderr,
                "source: SCHED_FIFO ceiling %d is below the URGENT priority %d\n",
                ceiling, SIGNAL_PRIO_URGENT);
        return EXIT_FAILURE;
    }
    printf("source: SCHED_FIFO ceiling %d, live signals to %s\n",
           ceiling, next_name);
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
    SIGEV_PULSE_INIT(&event, timer_coid, SIGNAL_PRIO_URGENT,
                     _PULSE_CODE_MINAVAIL, 0);
    if (MsgRegisterEvent(&event, timer_coid) == -1) {
        fprintf(stderr, "source: registering the timer pulse failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    if (timer_create(CLOCK_MONOTONIC, &event, &ticker) == -1) {
        fprintf(stderr, "source: creating the timer failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    memset(&spec, 0, sizeof spec);
    spec.it_value.tv_nsec = SOURCE_TICK_NS;
    spec.it_interval.tv_nsec = SOURCE_TICK_NS;
    if (timer_settime(ticker, 0, &spec, NULL) == -1) {
        fprintf(stderr, "source: starting the timer failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
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

    memset(&manifest, 0, sizeof manifest);
    manifest.type = SIGNAL_MSG_MANIFEST;
    next_ns[0] = 0;
    next_ns[1] = 0;
    next_ns[2] = 0;
    memset(&g_live, 0, sizeof g_live);
    g_live.coid = coid;
    g_live.manifest = &manifest;
    g_live.recorded = &recorded;
    g_live.number = &number;
    g_live.full = &full;
    if (pthread_create(&g_flood_thread, NULL, flood_thread, NULL) != 0) {
        fprintf(stderr, "source: creating the flood thread failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    g_flood_started = 1;

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
        if (set_fifo(SIGNAL_PRIO_OWN) != 0) {
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
        if (pulse.code != _PULSE_CODE_MINAVAIL) {
            continue;
        }

        ctl_lock();
        stop = g_ctl.stop;
        ctl_unlock();
        if (stop) {
            break;
        }

        if (!full) {
            for (;;) {
                uint32_t class_id;
                int64_t now;

                ctl_lock();
                stop = g_ctl.stop;
                ctl_unlock();
                if (stop || full) {
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
                    int issued = issue_one(coid, &manifest, &recorded, &number,
                                           class_id, &full);

                    if (issued < 0) {
                        failed = 1;
                        break;
                    }
                    if (issued == 0) {
                        break;
                    }
                }
            }
        }
        if (failed) {
            break;
        }
    }

    pthread_mutex_lock(&g_issue_lock);
    g_live.stop = 1;
    if (g_live.failed) {
        failed = 1;
    }
    pthread_mutex_unlock(&g_issue_lock);
    if (g_flood_started) {
        pthread_join(g_flood_thread, NULL);
        g_flood_started = 0;
    }

    memset(&spec, 0, sizeof spec);
    if (timer_settime(ticker, 0, &spec, NULL) == -1) {
        fprintf(stderr, "source: stopping the timer failed: %s\n",
                strerror(errno));
        failed = 1;
    }
    if (send_manifest(coid, &manifest, recorded) != 0) {
        name_close(coid);
        return EXIT_FAILURE;
    }
    if (name_close(coid) == -1) {
        fprintf(stderr, "source: closing the connection to %s failed: %s\n",
                next_name, strerror(errno));
        return EXIT_FAILURE;
    }
    printf("source: sent %lu signals\n", recorded);
    fflush(stdout);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
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

    if (install_alarm() != 0 || pin_cpu0() != 0) {
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
