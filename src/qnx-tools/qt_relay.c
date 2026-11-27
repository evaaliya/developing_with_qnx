/*
 * relay of the "QNX tools" network (D-098 (3), (4)).
 *
 * The name comes from name_attach on a dispatch handle; a thread pool
 * (thread_pool_create, thread_pool_start) with the numbers from the
 * QNX 8.0 example (lo_water 2, hi_water 4, increment 1, maximum 50)
 * serves the channel. A pool thread takes one message, stamps the hop,
 * sends it on with MsgSend (no time limit: the relay has none in
 * D-098) and replies to the sender with the next node's status after
 * the next node answered. No queue of its own, no choice by class: the
 * kernel queues waiting senders by priority and the receiving thread
 * inherits the sender's priority (the channel has no
 * _NTO_CHF_FIXED_PRIORITY). When every pool thread is busy, senders
 * wait in SEND: nothing is dropped and nothing is counted.
 *
 * The channel keeps the name_attach flags (_NTO_CHF_UNBLOCK among them),
 * as in Nerve (D-016, D-028; human decision on the D-098 note). An
 * unblock pulse is therefore not answered early: the sender is answered
 * only by the pool thread that holds its message, so a reply never goes
 * to a receive id that was already freed and reused (D-071).
 *
 *   relay [-o FILE] [-q] <attach-name> <next-name> [-q N] [-r]
 */
#define _QNX_SOURCE
/* The pool hands dispatch contexts to its functions, as in the
 * thread_pool_create example; must come before sys/dispatch.h. */
#define THREAD_POOL_PARAM_T dispatch_context_t

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
_Static_assert(QT_MSG_END > SIGNAL_MSG_TYPE,
               "QT_MSG_END must be inside the attached message range");

#define POOL_LO_WATER 2
#define POOL_HI_WATER 4
#define POOL_INCREMENT 1
#define POOL_MAXIMUM 50
/* Used only when the guest cannot make a pool of POOL_MAXIMUM (B35 rule). */
#define POOL_MAXIMUM_FALLBACK 16
#define FIND_ATTEMPTS 40
#define FIND_PAUSE_NS 50000000L

union relay_msg {
    uint16_t type;
    struct qt_end end;
    struct signal_message signal;
};

struct relay {
    const char *own_name;
    const char *next_name;
    pthread_mutex_t lock;
    int next_coid;
};

static struct relay g_relay;

static int64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "relay %s: reading the monotonic clock failed: %s\n",
                g_relay.own_name, strerror(errno));
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int pin_cpu0(void)
{
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "relay %s: pinning a thread to CPU 0 failed: %s\n",
                g_relay.own_name, strerror(errno));
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
        fprintf(stderr, "relay %s: setting priority %d failed: %s\n",
                g_relay.own_name, priority, strerror(rc));
        return -1;
    }
    return 0;
}

static int open_next_wait(const char *name)
{
    int attempt;

    for (attempt = 0; attempt < FIND_ATTEMPTS; attempt++) {
        struct timespec pause = { 0, FIND_PAUSE_NS };
        int coid = name_open(name, 0);

        if (coid != -1) {
            return coid;
        }
        if (errno != ENOENT) {
            return -1;
        }
        nanosleep(&pause, NULL);
    }
    errno = ENOENT;
    return -1;
}

/* The connection to the next node; opened again after it died (D-098 (6)). */
static int next_coid(struct relay *r, int *err)
{
    int coid;

    pthread_mutex_lock(&r->lock);
    if (r->next_coid == -1) {
        r->next_coid = name_open(r->next_name, 0);
        if (r->next_coid == -1) {
            *err = errno;
            pthread_mutex_unlock(&r->lock);
            return -1;
        }
        printf("relay %s: reopened %s coid %d\n", r->own_name, r->next_name,
               r->next_coid);
        fflush(stdout);
    }
    coid = r->next_coid;
    pthread_mutex_unlock(&r->lock);
    return coid;
}

static void drop_coid(struct relay *r, int coid)
{
    pthread_mutex_lock(&r->lock);
    if (r->next_coid == coid) {
        (void)name_close(coid);
        r->next_coid = -1;
        printf("relay %s: link down %s coid %d\n", r->own_name, r->next_name,
               coid);
        fflush(stdout);
    }
    pthread_mutex_unlock(&r->lock);
}

/* One message, in a pool thread: on to the next node, then the reply. */
static int on_message(message_context_t *ctp, int code, unsigned flags,
                      void *handle)
{
    struct relay *r = handle;
    union relay_msg msg;
    struct signal_reply reply;
    size_t len;
    long sent;
    int coid;
    int err = 0;

    (void)code;
    (void)flags;
    len = (size_t)ctp->info.msglen;
    if (len > sizeof msg) {
        len = sizeof msg;
    }
    if (len < sizeof(uint16_t) ||
        MsgRead(ctp->rcvid, &msg, len, 0) != (ssize_t)len) {
        (void)MsgError(ctp->rcvid, EINVAL);
        return 0;
    }
    if (msg.type == SIGNAL_MSG_TYPE) {
        if (len < sizeof msg.signal ||
            msg.signal.body.hop_count >= SIGNAL_MAX_HOPS) {
            (void)MsgError(ctp->rcvid, EINVAL);
            return 0;
        }
        msg.signal.body.hop_ns[msg.signal.body.hop_count] = monotonic_ns();
        msg.signal.body.hop_count++;
    } else if (msg.type != QT_MSG_END) {
        (void)MsgError(ctp->rcvid, ENOSYS);
        return 0;
    }

    coid = next_coid(r, &err);
    if (coid == -1) {
        fprintf(stderr, "relay %s: %s is not there: %s\n", r->own_name,
                r->next_name, strerror(err));
        (void)MsgError(ctp->rcvid, err);
        return 0;
    }
    memset(&reply, 0, sizeof reply);
    sent = MsgSend(coid, &msg, len, &reply, sizeof reply);
    if (sent == -1) {
        err = errno;
        if (msg.type == SIGNAL_MSG_TYPE) {
            fprintf(stderr, "relay %s: sending signal %" PRIu64 " to %s "
                    "failed: %s\n", r->own_name, msg.signal.body.number,
                    r->next_name, strerror(err));
        } else {
            fprintf(stderr, "relay %s: sending the end to %s failed: %s\n",
                    r->own_name, r->next_name, strerror(err));
        }
        if (err == ESRCH || err == EBADF || err == ENOTCONN) {
            drop_coid(r, coid);
        }
        (void)MsgError(ctp->rcvid, err);
        return 0;
    }
    if (MsgReply(ctp->rcvid, sent, &reply, sizeof reply) == -1) {
        fprintf(stderr, "relay %s: replying to the sender failed: %s\n",
                r->own_name, strerror(errno));
    }
    if (msg.type == QT_MSG_END) {
        printf("relay %s: passed the end of the run\n", r->own_name);
        fflush(stdout);
    }
    return 0;
}

/* A client went away: free its connection, as in the name_attach example. */
static int on_disconnect(message_context_t *ctp, int code, unsigned flags,
                         void *handle)
{
    (void)code;
    (void)flags;
    (void)handle;
    if (ConnectDetach(ctp->msg->pulse.scoid) == -1) {
        fprintf(stderr, "relay %s: detaching a departed client failed: %s\n",
                g_relay.own_name, strerror(errno));
    }
    return 0;
}

/*
 * A sender's time limit ran out while its message is held here. It is
 * answered only by the pool thread holding it (see the top of the file).
 */
static int on_unblock(message_context_t *ctp, int code, unsigned flags,
                      void *handle)
{
    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;
    return 0;
}

/* Each new pool thread is pinned to CPU 0 like the Nerve nodes (D-036). */
static THREAD_POOL_PARAM_T *context_alloc(THREAD_POOL_HANDLE_T *handle)
{
    (void)pin_cpu0();
    return dispatch_context_alloc((dispatch_t *)handle);
}

static int redirect(const char *path)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);

    if (fd == -1) {
        fprintf(stderr, "relay: opening %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1 || dup2(fd, STDERR_FILENO) == -1) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    struct qt_opts opts;
    thread_pool_attr_t pool_attr;
    thread_pool_t *tpp;
    message_attr_t mattr;
    name_attach_t *attach;
    dispatch_t *dpp;
    int first;
    int ceiling;
    int maximum = POOL_MAXIMUM;

    first = qt_parse_opts(argc, argv, &opts);
    if (first < 0 ||
        qt_parse_relay(argc, argv, first, &g_relay.own_name,
                       &g_relay.next_name) != 0) {
        fprintf(stderr, "usage: relay [-o FILE] [-q] <attach-name> <next-name> "
                "[-q N] [-r]\n");
        return EXIT_FAILURE;
    }
    if (opts.out != NULL && redirect(opts.out) != 0) {
        return EXIT_FAILURE;
    }
    if (opts.quiet) {
        setenv("SIGNAL_QUIET", "1", 1);
    }
    pthread_mutex_init(&g_relay.lock, NULL);
    g_relay.next_coid = -1;
    if (pin_cpu0() != 0 || set_fifo(SIGNAL_PRIO_OWN) != 0) {
        return EXIT_FAILURE;
    }
    ceiling = sched_get_priority_max(SCHED_FIFO);
    if (ceiling < SIGNAL_PRIO_URGENT) {
        fprintf(stderr,
                "relay %s: SCHED_FIFO ceiling %d is below the URGENT priority %d\n",
                g_relay.own_name, ceiling, SIGNAL_PRIO_URGENT);
        return EXIT_FAILURE;
    }
    printf("relay %s: SCHED_FIFO ceiling %d, own %d, pool lo_water %d "
           "hi_water %d increment %d maximum %d\n",
           g_relay.own_name, ceiling, SIGNAL_PRIO_OWN, POOL_LO_WATER,
           POOL_HI_WATER, POOL_INCREMENT, POOL_MAXIMUM);
    /* No queue of its own: the build line says 0. */
    printf("relay %s: build %s, queue 0\n", g_relay.own_name, QT_BUILD_NAME);
    printf("relay %s: pid %d start_ns %lld\n", g_relay.own_name, (int)getpid(),
           (long long)monotonic_ns());
    fflush(stdout);

    dpp = dispatch_create();
    if (dpp == NULL) {
        fprintf(stderr, "relay %s: dispatch_create failed: %s\n",
                g_relay.own_name, strerror(errno));
        return EXIT_FAILURE;
    }
    memset(&mattr, 0, sizeof mattr);
    mattr.nparts_max = 1;
    mattr.msg_max_size = sizeof(union relay_msg);
    if (message_attach(dpp, &mattr, SIGNAL_MSG_TYPE, QT_MSG_END, on_message,
                       &g_relay) == -1) {
        fprintf(stderr, "relay %s: message_attach failed: %s\n",
                g_relay.own_name, strerror(errno));
        return EXIT_FAILURE;
    }
    if (pulse_attach(dpp, 0, _PULSE_CODE_DISCONNECT, on_disconnect, NULL) == -1 ||
        pulse_attach(dpp, 0, _PULSE_CODE_UNBLOCK, on_unblock, NULL) == -1) {
        fprintf(stderr, "relay %s: pulse_attach failed: %s\n",
                g_relay.own_name, strerror(errno));
        return EXIT_FAILURE;
    }
    attach = name_attach(dpp, g_relay.own_name, 0);
    if (attach == NULL) {
        fprintf(stderr, "relay %s: could not register the name: %s\n",
                g_relay.own_name, strerror(errno));
        return EXIT_FAILURE;
    }
    g_relay.next_coid = open_next_wait(g_relay.next_name);
    if (g_relay.next_coid == -1) {
        fprintf(stderr, "relay %s: %s is not there yet (%s); opened at the "
                "first message\n", g_relay.own_name, g_relay.next_name,
                strerror(errno));
    }

    for (;;) {
        memset(&pool_attr, 0, sizeof pool_attr);
        pool_attr.handle = dpp;
        pool_attr.context_alloc = context_alloc;
        pool_attr.block_func = dispatch_block;
        pool_attr.unblock_func = dispatch_unblock;
        pool_attr.handler_func = dispatch_handler;
        pool_attr.context_free = dispatch_context_free;
        pool_attr.lo_water = POOL_LO_WATER;
        pool_attr.hi_water = POOL_HI_WATER;
        pool_attr.increment = POOL_INCREMENT;
        pool_attr.maximum = maximum;
        tpp = thread_pool_create(&pool_attr, POOL_FLAG_EXIT_SELF);
        if (tpp != NULL) {
            break;
        }
        if (maximum == POOL_MAXIMUM_FALLBACK) {
            fprintf(stderr, "relay %s: thread_pool_create failed: %s\n",
                    g_relay.own_name, strerror(errno));
            return EXIT_FAILURE;
        }
        fprintf(stderr, "relay %s: thread_pool_create with maximum %d failed: "
                "%s; trying %d\n", g_relay.own_name, maximum, strerror(errno),
                POOL_MAXIMUM_FALLBACK);
        maximum = POOL_MAXIMUM_FALLBACK;
    }
    printf("relay %s: attached, pool maximum %d\n", g_relay.own_name, maximum);
    fflush(stdout);
    if (thread_pool_start(tpp) == -1) {
        fprintf(stderr, "relay %s: thread_pool_start failed: %s\n",
                g_relay.own_name, strerror(errno));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
