#define _QNX_SOURCE

#include "close_list.h"
#include "drop_book.h"
#include "node_store.h"
#include "signal_msg.h"
#include "signal_queue.h"
#include "signal_sched.h"
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

/*
 * A missed name_open used to wait 50 ms. The whole burst fits in that
 * pause, so every URGENT in the burst paid it (D-055). 1 ms still
 * covers about two seconds over FIND_ATTEMPTS.
 */
#define FIND_ATTEMPTS 2000
#define FIND_PAUSE_NS 1000000L
#define NAME_LIMIT 64

struct relay {
    const char *own_name;
    const char *next_name;
    struct signal_queue queue;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    uint64_t next_order;
    int own_priority;
    pthread_t forward;
    int forward_prio;
    sig_atomic_t boosts;
    sig_atomic_t repicks;
    int next_coid;
    int link_down;
    int manifest_waiting;
    rcvid_t manifest_rcvid;
    struct signal_manifest manifest;
    size_t capacity;
    /*
     * Numbers this relay dropped that no close or manifest has carried
     * yet. Shared memory, so a restarted relay keeps them (D-063).
     */
    pthread_mutex_t drop_lock;
    struct drop_book *drops;
    /* One close at a time, passed on by the close thread (D-062). */
    pthread_t closer;
    pthread_cond_t close_ready;
    int close_waiting;
    rcvid_t close_rcvid;
    struct signal_close close;
};

static int set_fifo(const char *who, int priority)
{
    struct sched_param param;
    int rc;

    memset(&param, 0, sizeof param);
    param.sched_priority = priority;
    rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        fprintf(stderr, "relay %s: setting priority %d failed: %s\n",
                who, priority, strerror(rc));
        return -1;
    }
    return 0;
}

static struct relay *stop_relay;
static volatile sig_atomic_t boosts_printed;

/* One line at the end. A line per boost would move the latency numbers. */
static void emit_boosts(void)
{
    char line[128];
    int n;

    if (stop_relay == NULL || boosts_printed) {
        return;
    }
    boosts_printed = 1;
    n = snprintf(line, sizeof line, "relay %s: boosts %d\n",
                 stop_relay->own_name, (int)stop_relay->boosts);
    if (n > 0 && n < (int)sizeof line) {
        if (write(STDOUT_FILENO, line, (size_t)n) < 0) {
            return;
        }
    }
    n = snprintf(line, sizeof line, "relay %s: repicks %d\n",
                 stop_relay->own_name, (int)stop_relay->repicks);
    if (n > 0 && n < (int)sizeof line) {
        if (write(STDOUT_FILENO, line, (size_t)n) < 0) {
            return;
        }
    }
}

static void on_stop(int signo)
{
    (void)signo;
    emit_boosts();
    _exit(EXIT_SUCCESS);
}

/* The caller holds node->lock. Raise the forward thread, then wake it. */
static void wake_forward_locked(struct relay *node)
{
    int top = signal_queue_top_priority(&node->queue);

    if (top > node->forward_prio) {
        int rc = pthread_setschedprio(node->forward, top);

        if (rc != 0) {
            fprintf(stderr, "relay %s: setting priority %d failed: %s\n",
                    node->own_name, top, strerror(rc));
            exit(EXIT_FAILURE);
        }
        node->forward_prio = top;
        node->boosts++;
    }
    pthread_cond_signal(&node->ready);
}

/* The caller holds node->lock. This thread is the forward thread. */
static void settle_forward_locked(struct relay *node)
{
    int top = signal_queue_top_priority(&node->queue);
    int next = node->own_priority;

    if (top > next) {
        next = top;
    }
    if (set_fifo(node->own_name, next) != 0) {
        exit(EXIT_FAILURE);
    }
    node->forward_prio = next;
}

static int pin_cpu0(const char *who)
{
    /* Bit 0 is CPU 0. The mask is the pointer value, not a pointer. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "relay %s: pinning this thread to CPU 0 failed: %s\n",
                who, strerror(errno));
        return -1;
    }
    return 0;
}

static int64_t monotonic_ns(const char *who)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "relay %s: reading the monotonic clock failed: %s\n",
                who, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec > 999999999L ||
        ts.tv_sec > (INT64_MAX / 1000000000LL)) {
        fprintf(stderr,
                "relay %s: the monotonic clock value does not fit in a signed "
                "64-bit nanosecond count\n",
                who);
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
        if (errno != ENOENT && errno != ESRCH) {
            fprintf(stderr, "relay: opening the next name %s failed: %s\n",
                    name, strerror(errno));
            return -1;
        }
        pause.tv_sec = 0;
        pause.tv_nsec = FIND_PAUSE_NS;
        nanosleep(&pause, NULL);
    }

    fprintf(stderr, "relay: could not find a node registered as %s after 2 seconds\n",
            name);
    return -1;
}

static void write_lost(const struct relay *node, const struct signal *sig)
{
    printf("relay %s: lost signal %" PRIu64 " class %s\n",
           node->own_name, sig->number, signal_class_name(sig->class_id));
    fflush(stdout);
}

/* Receive thread only. The forward thread reads the marked manifest. */
static void note_drop(struct relay *node, uint64_t number, uint32_t class_id)
{
    if (!signal_log_quiet()) {
        printf("relay %s: dropped signal %" PRIu64 " class %s at %lld ns\n",
               node->own_name, number, signal_class_name(class_id),
               (long long)monotonic_ns(node->own_name));
        fflush(stdout);
    }
    pthread_mutex_lock(&node->drop_lock);
    if (drop_book_add(node->drops, number) != 0) {
        pthread_mutex_unlock(&node->drop_lock);
        fprintf(stderr,
                "relay %s: the dropped list is full, signal %" PRIu64
                " will not be marked on the manifest\n",
                node->own_name, number);
        return;
    }
    pthread_mutex_unlock(&node->drop_lock);
}

static void mark_dropped(struct relay *node, struct signal_manifest *manifest)
{
    uint16_t i;
    uint16_t count = manifest->count;

    if (count > SIGNAL_MANIFEST_CAP) {
        count = SIGNAL_MANIFEST_CAP;
    }
    pthread_mutex_lock(&node->drop_lock);
    for (i = 0; i < count; i++) {
        unsigned d;

        for (d = 0; d < node->drops->count; d++) {
            if (manifest->item[i].number != node->drops->number[d]) {
                continue;
            }
            manifest->item[i].reserved = SIGNAL_MANIFEST_DROPPED;
            break;
        }
    }
    pthread_mutex_unlock(&node->drop_lock);
}

/* The caller holds node->lock. */
static void drain_lost_locked(struct relay *node)
{
    struct signal_queue_item item;

    while (signal_queue_pop(&node->queue, &item) == 0) {
        write_lost(node, &item.body);
    }
}

static int reopen_next(struct relay *node)
{
    for (;;) {
        int coid = open_next(node->next_name);

        if (coid != -1) {
            printf("relay %s: reopened %s coid %d\n",
                   node->own_name, node->next_name, coid);
            fflush(stdout);
            return coid;
        }
        pthread_mutex_lock(&node->lock);
        drain_lost_locked(node);
        pthread_mutex_unlock(&node->lock);
        fprintf(stderr, "relay %s: %s is not back yet\n",
                node->own_name, node->next_name);
    }
}

static void close_quietly(const struct relay *node, int coid)
{
    if (name_close(coid) == -1 && errno != EBADF) {
        fprintf(stderr, "relay %s: closing the dead connection failed: %s\n",
                node->own_name, strerror(errno));
    }
}

/* current is the signal whose send failed. NULL when nothing was in flight. */
static void send_failed(struct relay *node, int coid, const struct signal *current)
{
    int fresh;

    printf("relay %s: link down %s coid %d\n",
           node->own_name, node->next_name, coid);
    fflush(stdout);
    if (current != NULL) {
        write_lost(node, current);
    }
    pthread_mutex_lock(&node->lock);
    drain_lost_locked(node);
    node->next_coid = -1;
    node->link_down = 1;
    pthread_mutex_unlock(&node->lock);
    close_quietly(node, coid);
    fresh = reopen_next(node);
    pthread_mutex_lock(&node->lock);
    node->next_coid = fresh;
    node->link_down = 0;
    wake_forward_locked(node);
    pthread_mutex_unlock(&node->lock);
}

static void format_waiting(const struct signal_queue *q, char *buf, size_t buf_len)
{
    size_t used = 0;
    size_t i;

    if (buf_len == 0) {
        return;
    }
    buf[0] = '\0';
    for (i = 0; i < signal_queue_count(q); i++) {
        const struct signal_queue_item *item = signal_queue_item(q, i);
        int wrote;

        wrote = snprintf(buf + used, buf_len - used, "%ssignal %" PRIu64 " %s",
                         (i == 0) ? "" : ", ",
                         item->body.number,
                         signal_class_name(item->body.class_id));
        if (wrote < 0 || (size_t)wrote >= buf_len - used) {
            break;
        }
        used += (size_t)wrote;
    }
}

/*
 * Passes each close on to the next node, outside the signal queue, so a
 * close neither waits for an empty queue nor holds up a queued URGENT.
 * It runs at BACKGROUND priority: below URGENT and COMMAND, and not
 * starved by the flood, which also runs at BACKGROUND (D-062). The
 * previous node is answered only after the next node took the close,
 * so source keeps the numbers until the whole chain has them.
 */
static void *close_main(void *arg)
{
    struct relay *node = arg;
    static struct signal_close close;

    if (pin_cpu0(node->own_name) != 0 ||
        set_fifo(node->own_name, SIGNAL_PRIO_BACKGROUND) != 0) {
        exit(EXIT_FAILURE);
    }
    for (;;) {
        rcvid_t rcvid;
        int coid;
        int marked;
        long sent;
        unsigned n;

        pthread_mutex_lock(&node->lock);
        while (!node->close_waiting) {
            pthread_cond_wait(&node->close_ready, &node->lock);
        }
        close = node->close;
        rcvid = node->close_rcvid;
        coid = node->next_coid;
        pthread_mutex_unlock(&node->lock);
        n = close.count;

        pthread_mutex_lock(&node->drop_lock);
        marked = close_mark_dropped(close.item, n, node->drops->number,
                                    node->drops->count);
        pthread_mutex_unlock(&node->drop_lock);
        if (marked < 0) {
            fprintf(stderr, "relay %s: a close is not in number order\n",
                    node->own_name);
            sent = EINVAL;
        } else if (coid < 0) {
            sent = EAGAIN;
        } else {
            sent = MsgSend(coid, &close, sizeof close, NULL, 0);
            if (sent == -1) {
                sent = errno;
                fprintf(stderr, "relay %s: passing a close to %s failed: %s\n",
                        node->own_name, node->next_name, strerror(errno));
            }
        }
        if (sent == EOK) {
            pthread_mutex_lock(&node->drop_lock);
            drop_book_forget(node->drops, close.item[0].number,
                             close.item[n - 1].number);
            pthread_mutex_unlock(&node->drop_lock);
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "relay %s: replying to a close failed: %s\n",
                        node->own_name, strerror(errno));
            }
            printf("relay %s: passed close of %u signals %" PRIu64 "..%" PRIu64
                   ", marked dropped %d\n",
                   node->own_name, n, close.item[0].number,
                   close.item[n - 1].number, marked);
            fflush(stdout);
        } else if (MsgError(rcvid, (int)sent) == -1) {
            fprintf(stderr, "relay %s: rejecting a close failed: %s\n",
                    node->own_name, strerror(errno));
        }
        pthread_mutex_lock(&node->lock);
        node->close_waiting = 0;
        pthread_mutex_unlock(&node->lock);
    }
    return NULL;
}

static void *forward_main(void *arg)
{
    struct relay *node = arg;
    int coid;

    if (pin_cpu0(node->own_name) != 0 ||
        set_fifo(node->own_name, node->own_priority) != 0) {
        exit(EXIT_FAILURE);
    }

    coid = open_next(node->next_name);
    if (coid == -1) {
        exit(EXIT_FAILURE);
    }
    pthread_mutex_lock(&node->lock);
    node->next_coid = coid;
    node->link_down = 0;
    pthread_mutex_unlock(&node->lock);

    for (;;) {
        struct signal_queue_item item;
        static struct signal_manifest manifest;
        char waiting[4096];
        struct signal_message message;
        struct signal_reply reply;
        int have_item = 0;
        int have_manifest = 0;
        rcvid_t manifest_rcvid = 0;
        int priority;
        long sent;
        int64_t arrived_ns = 0;
        int64_t picked_ns = 0;
        int64_t start_ns = 0;
        int64_t end_ns = 0;

        pthread_mutex_lock(&node->lock);
        while (signal_queue_count(&node->queue) == 0 && !node->manifest_waiting &&
               node->next_coid >= 0) {
            pthread_cond_wait(&node->ready, &node->lock);
        }
        coid = node->next_coid;
        if (coid < 0) {
            drain_lost_locked(node);
            pthread_mutex_unlock(&node->lock);
            coid = reopen_next(node);
            pthread_mutex_lock(&node->lock);
            node->next_coid = coid;
            node->link_down = 0;
            pthread_mutex_unlock(&node->lock);
            continue;
        }
        if (signal_queue_count(&node->queue) > 0) {
            if (signal_queue_pop(&node->queue, &item) == 0) {
                have_item = 1;
                format_waiting(&node->queue, waiting, sizeof waiting);
            }
        } else if (node->manifest_waiting) {
            manifest = node->manifest;
            manifest_rcvid = node->manifest_rcvid;
            node->manifest_waiting = 0;
            have_manifest = 1;
        }
        pthread_mutex_unlock(&node->lock);

        if (have_manifest) {
            sent = MsgSend(coid, &manifest, sizeof manifest, NULL, 0);
            pthread_mutex_lock(&node->lock);
            settle_forward_locked(node);
            pthread_mutex_unlock(&node->lock);
            if (sent == -1) {
                pthread_mutex_lock(&node->lock);
                node->manifest = manifest;
                node->manifest_rcvid = manifest_rcvid;
                node->manifest_waiting = 1;
                pthread_mutex_unlock(&node->lock);
                fprintf(stderr,
                        "relay %s: sending the manifest to %s failed: %s\n",
                        node->own_name, node->next_name, strerror(errno));
                send_failed(node, coid, NULL);
                continue;
            }
            if (sent != EOK) {
                fprintf(stderr,
                        "relay %s: %s rejected the manifest with status %ld\n",
                        node->own_name, node->next_name, sent);
                if (MsgError(manifest_rcvid, (int)sent) == -1) {
                    fprintf(stderr,
                            "relay %s: rejecting the manifest failed: %s\n",
                            node->own_name, strerror(errno));
                }
                continue;
            }
            if (MsgReply(manifest_rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "relay %s: replying to the manifest failed: %s\n",
                        node->own_name, strerror(errno));
                exit(EXIT_FAILURE);
            }
            printf("relay %s: forwarded manifest of %u signals\n",
                   node->own_name, manifest.count);
            fflush(stdout);
            continue;
        }

        if (!have_item) {
            continue;
        }

        priority = signal_class_priority(item.body.class_id);
        if (priority < 0 || set_fifo(node->own_name, priority) != 0) {
            fprintf(stderr, "relay %s: signal %" PRIu64 " has no class priority\n",
                    node->own_name, item.body.number);
            exit(EXIT_FAILURE);
        }

        memset(&message, 0, sizeof message);
        message.type = SIGNAL_MSG_TYPE;
        message.body = item.body;
        memset(&reply, 0, sizeof reply);
        /*
         * arrived is this node's own hop stamp (D-048). start is the
         * clock immediately before MsgSend, end immediately after.
         * picked is the clock under the lock after the last repick
         * and before that send (D-051).
         */
        if (item.body.hop_count >= 1 &&
            item.body.hop_count <= SIGNAL_MAX_HOPS) {
            arrived_ns = item.body.hop_ns[item.body.hop_count - 1];
        } else {
            arrived_ns = 0;
        }
        {
            int exchanged = 0;
            int repick_rc;

            pthread_mutex_lock(&node->lock);
            while ((repick_rc = signal_queue_repick(&node->queue, &item)) == 1) {
                node->repicks++;
                exchanged = 1;
            }
            picked_ns = monotonic_ns(node->own_name);
            if (exchanged) {
                format_waiting(&node->queue, waiting, sizeof waiting);
            }
            pthread_mutex_unlock(&node->lock);
            if (repick_rc < 0) {
                fprintf(stderr, "relay %s: choosing again for signal %" PRIu64
                        " failed\n",
                        node->own_name, item.body.number);
                exit(EXIT_FAILURE);
            }
            if (exchanged) {
                priority = signal_class_priority(item.body.class_id);
                if (priority < 0 || set_fifo(node->own_name, priority) != 0) {
                    fprintf(stderr,
                            "relay %s: signal %" PRIu64 " has no class priority\n",
                            node->own_name, item.body.number);
                    exit(EXIT_FAILURE);
                }
                memset(&message, 0, sizeof message);
                message.type = SIGNAL_MSG_TYPE;
                message.body = item.body;
                memset(&reply, 0, sizeof reply);
                if (item.body.hop_count >= 1 &&
                    item.body.hop_count <= SIGNAL_MAX_HOPS) {
                    arrived_ns = item.body.hop_ns[item.body.hop_count - 1];
                } else {
                    arrived_ns = 0;
                }
            }
        }
        start_ns = monotonic_ns(node->own_name);
        sent = MsgSend(coid, &message, sizeof message, &reply, sizeof reply);
        end_ns = monotonic_ns(node->own_name);
        pthread_mutex_lock(&node->lock);
        settle_forward_locked(node);
        pthread_mutex_unlock(&node->lock);
        if (sent == -1) {
            fprintf(stderr, "relay %s: sending signal %" PRIu64 " to %s failed: %s\n",
                    node->own_name, item.body.number, node->next_name,
                    strerror(errno));
            send_failed(node, coid, &item.body);
            continue;
        }
        if (sent != EOK) {
            fprintf(stderr,
                    "relay %s: %s rejected signal %" PRIu64 " with status %ld\n",
                    node->own_name, node->next_name, item.body.number, sent);
            exit(EXIT_FAILURE);
        }

        if (!signal_log_quiet()) {
            printf("relay %s: sent signal %" PRIu64
                   " class %s; still waiting: %s arrived %" PRId64
                   " start %" PRId64 " end %" PRId64 " picked %" PRId64 "\n",
                   node->own_name, item.body.number,
                   signal_class_name(item.body.class_id),
                   waiting[0] == '\0' ? "none" : waiting,
                   arrived_ns, start_ns, end_ns, picked_ns);
            fflush(stdout);
        }
    }
    return NULL;
}

static int reject(const char *who, rcvid_t rcvid, int error, const char *what)
{
    if (MsgError(rcvid, error) == -1) {
        fprintf(stderr, "relay %s: %s: %s\n", who, what, strerror(errno));
        return -1;
    }
    return 0;
}

static int enqueue(struct relay *node, const struct signal *sig, int64_t arrival_ns)
{
    int rc;

    pthread_mutex_lock(&node->lock);
    if (node->link_down) {
        pthread_mutex_unlock(&node->lock);
        return 1;
    }
    rc = signal_queue_push(&node->queue, sig, arrival_ns, node->next_order);
    if (rc == 0) {
        node->next_order++;
        wake_forward_locked(node);
    }
    pthread_mutex_unlock(&node->lock);
    return rc;
}

/* Tell the next node now. The manifest still closes the books later. */
static void announce_dropped(struct relay *node, uint64_t number,
                             uint32_t class_id);

/*
 * One MsgSend carries a batch of BACKGROUND. Every signal is admitted
 * before the reply, and the forward thread is woken only after that
 * reply, so the batch lands faster than one forward hop (D-059).
 * Returns 0, or -1 when the process should exit.
 */
static int admit_batch(struct relay *node, const struct signal_batch *batch,
                       rcvid_t rcvid)
{
    unsigned n;
    unsigned i;
    int64_t now_ns;
    int queued_any = 0;
    struct signal dropped[SIGNAL_BATCH_MAX];
    struct signal lost[SIGNAL_BATCH_MAX];
    unsigned dropped_n = 0;
    unsigned lost_n = 0;
    unsigned waiting_before;

    if (batch->count > SIGNAL_BATCH_MAX) {
        fprintf(stderr, "relay %s: a batch of %u is larger than %d\n",
                node->own_name, batch->count, SIGNAL_BATCH_MAX);
        return reject(node->own_name, rcvid, EINVAL,
                      "rejecting an oversized batch failed");
    }
    n = batch->count;
    for (i = 0; i < n; i++) {
        if (batch->body[i].class_id != SIGNAL_CLASS_BACKGROUND ||
            batch->body[i].hop_count >= SIGNAL_MAX_HOPS) {
            fprintf(stderr, "relay %s: batch signal %" PRIu64
                    " is not a BACKGROUND signal with room for a hop\n",
                    node->own_name, batch->body[i].number);
            return reject(node->own_name, rcvid, EINVAL,
                          "rejecting a batch signal failed");
        }
    }

    now_ns = monotonic_ns(node->own_name);
    pthread_mutex_lock(&node->lock);
    waiting_before = signal_queue_count(&node->queue);
    for (i = 0; i < n; i++) {
        struct signal stamped = batch->body[i];

        stamped.hop_ns[stamped.hop_count] = now_ns;
        stamped.hop_count++;
        if (node->link_down) {
            if (lost_n < SIGNAL_BATCH_MAX) {
                lost[lost_n++] = stamped;
            }
            continue;
        }
        if (signal_queue_count(&node->queue) >= node->capacity) {
            if (dropped_n < SIGNAL_BATCH_MAX) {
                dropped[dropped_n++] = stamped;
            }
            continue;
        }
        if (signal_queue_push(&node->queue, &stamped, now_ns,
                              node->next_order) != 0) {
            pthread_mutex_unlock(&node->lock);
            fprintf(stderr, "relay %s: batch signal %" PRIu64
                    " was accepted but did not fit in the queue\n",
                    node->own_name, stamped.number);
            return -1;
        }
        node->next_order++;
        queued_any = 1;
    }
    pthread_mutex_unlock(&node->lock);

    for (i = 0; i < dropped_n; i++) {
        note_drop(node, dropped[i].number, dropped[i].class_id);
    }
    for (i = 0; i < lost_n; i++) {
        write_lost(node, &lost[i]);
    }
    if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
        fprintf(stderr, "relay %s: replying to a batch failed: %s\n",
                node->own_name, strerror(errno));
        return -1;
    }
    if (queued_any) {
        pthread_mutex_lock(&node->lock);
        wake_forward_locked(node);
        pthread_mutex_unlock(&node->lock);
    }
    /*
     * The queue occupancy at arrival shows whether the chain drained
     * between batches. A drained queue reads 0 (D-060).
     */
    if (!signal_log_quiet()) {
        printf("relay %s: batch %u queued %u dropped %u waiting before %u\n",
               node->own_name, n, n - dropped_n - lost_n, dropped_n,
               waiting_before);
        fflush(stdout);
    }
    /*
     * One notice per batch is enough to move sink to DEGRADED; the
     * manifest still marks every dropped number. One MsgSend per drop
     * kept this receive thread away from URGENT for a whole batch (D-060).
     */
    if (dropped_n > 0) {
        announce_dropped(node, dropped[0].number, dropped[0].class_id);
    }
    if (set_fifo(node->own_name, node->own_priority) != 0) {
        return -1;
    }
    return 0;
}

/* Tell the next node now. The manifest still closes the books later. */
static void announce_dropped(struct relay *node, uint64_t number,
                             uint32_t class_id)
{
    struct signal_dropped notice;
    int coid;
    long sent;

    pthread_mutex_lock(&node->lock);
    coid = node->next_coid;
    pthread_mutex_unlock(&node->lock);
    if (coid < 0) {
        return;
    }
    memset(&notice, 0, sizeof notice);
    notice.type = SIGNAL_MSG_DROPPED;
    notice.class_id = class_id;
    notice.number = number;
    sent = MsgSend(coid, &notice, sizeof notice, NULL, 0);
    if (sent == -1) {
        fprintf(stderr,
                "relay %s: telling the next node about dropped signal %"
                PRIu64 " failed: %s\n",
                node->own_name, number, strerror(errno));
        return;
    }
    if (sent != EOK) {
        fprintf(stderr,
                "relay %s: the next node rejected the drop notice for %"
                PRIu64 " with status %ld\n",
                node->own_name, number, sent);
    }
}

static int name_ok(const char *name)
{
    size_t len;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    len = strlen(name);
    if (len >= NAME_LIMIT || strchr(name, '/') != NULL) {
        return 0;
    }
    return 1;
}

static int parse_u(const char *text, unsigned long *out)
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

static int parse_relay_args(int argc, char **argv, const char **own,
                            const char **next, size_t *capacity, int *resume)
{
    const char *names[2];
    int name_count = 0;
    int got_q = 0;
    unsigned long value = SIGNAL_QUEUE_CAP;
    int i;

    *resume = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            *resume = 1;
            continue;
        }
        if (strcmp(argv[i], "-q") == 0) {
            if (got_q || i + 1 >= argc) {
                fprintf(stderr,
                        "usage: relay <attach-name> <next-name> [-q N] [-r]\n");
                return -1;
            }
            i++;
            if (parse_u(argv[i], &value) != 0 || value < 1 ||
                value > SIGNAL_QUEUE_CAP) {
                fprintf(stderr, "relay: queue capacity must be 1..%d\n",
                        SIGNAL_QUEUE_CAP);
                return -1;
            }
            got_q = 1;
            continue;
        }
        if (name_count >= 2 || !name_ok(argv[i])) {
            fprintf(stderr, "usage: relay <attach-name> <next-name> [-q N] [-r]\n");
            return -1;
        }
        names[name_count++] = argv[i];
    }
    if (name_count != 2) {
        fprintf(stderr, "usage: relay <attach-name> <next-name> [-q N] [-r]\n");
        return -1;
    }
    *own = names[0];
    *next = names[1];
    *capacity = (size_t)value;
    return 0;
}

/*
 * The drop list in shared memory (D-063). Without -r a new list. With
 * -r the dead relay's list, or a line saying it is gone. If shared
 * memory fails, the list lives in this process only, and that is said.
 */
static struct drop_book *open_drops(const char *own_name, int resume)
{
    static struct drop_book local;
    char store[NAME_LIMIT + 16];
    enum node_store_found found = NODE_STORE_NEW;
    struct drop_book *book = NULL;

    if (drop_book_store_name(own_name, store, sizeof store) == 0) {
        book = node_store_map(store, sizeof *book, resume, &found);
        if (book == NULL) {
            fprintf(stderr, "relay %s: shared memory %s failed: %s; the drop "
                    "list will not outlive this process\n",
                    own_name, store, strerror(errno));
        }
    }
    if (book == NULL) {
        book = &local;
        found = resume ? NODE_STORE_MISSING : NODE_STORE_NEW;
    }
    if (found == NODE_STORE_KEPT && drop_book_resume(book) == 0) {
        printf("relay %s: resumed the books of a dead relay (resume %u): %u "
               "dropped numbers kept; its queue is lost and source closes "
               "those numbers as lost\n",
               own_name, (unsigned)book->resumes, (unsigned)book->count);
        fflush(stdout);
        return book;
    }
    drop_book_init(book);
    if (resume) {
        printf("relay %s: books reset after a restart (%s); numbers the dead "
               "relay dropped will be counted lost\n", own_name,
               found == NODE_STORE_KEPT ? "saved books were not whole"
                                        : node_store_found_text(found));
        fflush(stdout);
    }
    return book;
}

int main(int argc, char **argv)
{
    static struct relay node;
    name_attach_t *attach;
    int ceiling;
    int create_rc;
    static union {
        struct _pulse pulse;
        struct signal_message message;
        struct signal_manifest manifest;
        struct signal_close close;
        struct signal_batch batch;
        struct signal_dropped dropped;
    } buffer;

    const char *own_name;
    const char *next_name;
    size_t capacity;
    int resume;

    if (parse_relay_args(argc, argv, &own_name, &next_name, &capacity,
                         &resume) != 0) {
        return EXIT_FAILURE;
    }

    memset(&node, 0, sizeof node);
    node.own_name = own_name;
    node.next_name = next_name;
    node.capacity = capacity;
    node.next_coid = -1;
    signal_queue_init(&node.queue);
    pthread_mutex_init(&node.lock, NULL);
    pthread_cond_init(&node.ready, NULL);
    pthread_mutex_init(&node.drop_lock, NULL);
    pthread_cond_init(&node.close_ready, NULL);
    node.next_order = 1;
    node.drops = open_drops(own_name, resume);

    if (pin_cpu0(node.own_name) != 0) {
        return EXIT_FAILURE;
    }
    if (set_fifo(node.own_name, SIGNAL_PRIO_OWN) != 0) {
        return EXIT_FAILURE;
    }
    {
        struct sched_param param;
        int policy = 0;
        int rc = pthread_getschedparam(pthread_self(), &policy, &param);
        if (rc != 0) {
            fprintf(stderr, "relay %s: reading the own priority failed: %s\n",
                    node.own_name, strerror(rc));
            return EXIT_FAILURE;
        }
        node.own_priority = param.sched_priority;
        node.forward_prio = node.own_priority;
    }
    stop_relay = &node;
    if (signal(SIGTERM, on_stop) == SIG_ERR || signal(SIGINT, on_stop) == SIG_ERR) {
        fprintf(stderr, "relay %s: could not catch the stop signal: %s\n",
                node.own_name, strerror(errno));
        return EXIT_FAILURE;
    }
    if (atexit(emit_boosts) != 0) {
        fprintf(stderr, "relay %s: could not register the boost line\n",
                node.own_name);
        return EXIT_FAILURE;
    }

    ceiling = sched_get_priority_max(SCHED_FIFO);
    if (ceiling < SIGNAL_PRIO_URGENT) {
        fprintf(stderr,
                "relay %s: SCHED_FIFO ceiling %d is below the URGENT priority %d\n",
                node.own_name, ceiling, SIGNAL_PRIO_URGENT);
        return EXIT_FAILURE;
    }
    printf("relay %s: SCHED_FIFO ceiling %d, own %d, BACKGROUND %d, COMMAND %d, "
           "URGENT %d\n",
           node.own_name, ceiling, node.own_priority, SIGNAL_PRIO_BACKGROUND,
           SIGNAL_PRIO_COMMAND, SIGNAL_PRIO_URGENT);
    fflush(stdout);

    attach = name_attach(NULL, node.own_name, 0);
    if (attach == NULL) {
        fprintf(stderr, "relay %s: could not register the name: %s\n",
                node.own_name, strerror(errno));
        return EXIT_FAILURE;
    }

    create_rc = pthread_create(&node.forward, NULL, forward_main, &node);
    if (create_rc != 0) {
        fprintf(stderr, "relay %s: could not start the forward thread: %s\n",
                node.own_name, strerror(create_rc));
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }
    create_rc = pthread_create(&node.closer, NULL, close_main, &node);
    if (create_rc != 0) {
        fprintf(stderr, "relay %s: could not start the close thread: %s\n",
                node.own_name, strerror(create_rc));
        name_detach(attach, 0);
        return EXIT_FAILURE;
    }

    for (;;) {
        struct _msg_info info;
        struct signal stamped;
        rcvid_t rcvid;
        int64_t now_ns;
        int full;

        memset(&buffer, 0, sizeof buffer);
        memset(&info, 0, sizeof info);
        rcvid = MsgReceive(attach->chid, &buffer, sizeof buffer, &info);
        if (rcvid == -1) {
            fprintf(stderr, "relay %s: waiting for a signal failed: %s\n",
                    node.own_name, strerror(errno));
            return EXIT_FAILURE;
        }

        if (rcvid == 0) {
            if (buffer.pulse.code == _PULSE_CODE_DISCONNECT) {
                if (ConnectDetach(buffer.pulse.scoid) == -1) {
                    fprintf(stderr,
                            "relay %s: detaching a departed client failed: %s\n",
                            node.own_name, strerror(errno));
                }
            } else if (buffer.pulse.code == _PULSE_CODE_UNBLOCK) {
                if (MsgError(buffer.pulse.value.sival_int, EINTR) == -1) {
                    fprintf(stderr,
                            "relay %s: releasing a client that asked to unblock "
                            "failed: %s\n",
                            node.own_name, strerror(errno));
                }
            } else if (buffer.pulse.code == _PULSE_CODE_COIDDEATH) {
                int coid = buffer.pulse.value.sival_int;
                int closed = -1;

                /*
                 * ChannelCreate's example compares pulse->type. That field
                 * is _PULSE_TYPE (0). The death code lives in pulse.code.
                 * A stale pulse still names a live connection.
                 */
                if (ConnectServerInfo(0, coid, NULL) == coid) {
                    printf("relay %s: stale death pulse coid %d\n",
                           node.own_name, coid);
                    fflush(stdout);
                } else {
                    pthread_mutex_lock(&node.lock);
                    if (coid == node.next_coid) {
                        closed = node.next_coid;
                        node.next_coid = -1;
                        node.link_down = 1;
                        wake_forward_locked(&node);
                    }
                    pthread_mutex_unlock(&node.lock);
                    if (closed != -1) {
                        printf("relay %s: link down %s coid %d\n",
                               node.own_name, node.next_name, closed);
                        fflush(stdout);
                        close_quietly(&node, closed);
                    }
                }
            }
            continue;
        }

        if (info.msglen < (ssize_t)sizeof(uint16_t) ||
            buffer.message.type <= _IO_MAX) {
            if (reject(node.own_name, rcvid, ENOSYS,
                       "rejecting a system message failed") != 0) {
                return EXIT_FAILURE;
            }
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.message.type == SIGNAL_MSG_MANIFEST) {
            if (info.msglen < (ssize_t)sizeof(struct signal_manifest) ||
                buffer.manifest.count > SIGNAL_MANIFEST_CAP) {
                fprintf(stderr, "relay %s: the manifest is the wrong size\n",
                        node.own_name);
                if (reject(node.own_name, rcvid, EINVAL,
                           "rejecting the manifest failed") != 0) {
                    return EXIT_FAILURE;
                }
                if (set_fifo(node.own_name, node.own_priority) != 0) {
                    return EXIT_FAILURE;
                }
                continue;
            }
            pthread_mutex_lock(&node.lock);
            if (node.manifest_waiting) {
                pthread_mutex_unlock(&node.lock);
                if (reject(node.own_name, rcvid, EBUSY,
                           "rejecting a second manifest failed") != 0) {
                    return EXIT_FAILURE;
                }
                if (set_fifo(node.own_name, node.own_priority) != 0) {
                    return EXIT_FAILURE;
                }
                continue;
            }
            node.manifest = buffer.manifest;
            mark_dropped(&node, &node.manifest);
            node.manifest_rcvid = rcvid;
            node.manifest_waiting = 1;
            wake_forward_locked(&node);
            pthread_mutex_unlock(&node.lock);
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.message.type == SIGNAL_MSG_CLOSE) {
            if (info.msglen < (ssize_t)sizeof(struct signal_close) ||
                buffer.close.count == 0 ||
                buffer.close.count > SIGNAL_CLOSE_CAP) {
                fprintf(stderr, "relay %s: the close is the wrong size\n",
                        node.own_name);
                if (reject(node.own_name, rcvid, EINVAL,
                           "rejecting the close failed") != 0) {
                    return EXIT_FAILURE;
                }
            } else {
                pthread_mutex_lock(&node.lock);
                if (node.close_waiting) {
                    pthread_mutex_unlock(&node.lock);
                    if (reject(node.own_name, rcvid, EBUSY,
                               "rejecting a second close failed") != 0) {
                        return EXIT_FAILURE;
                    }
                } else {
                    node.close = buffer.close;
                    node.close_rcvid = rcvid;
                    node.close_waiting = 1;
                    pthread_cond_signal(&node.close_ready);
                    pthread_mutex_unlock(&node.lock);
                }
            }
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.message.type == SIGNAL_MSG_DROPPED) {
            if (info.msglen < (ssize_t)sizeof(struct signal_dropped)) {
                fprintf(stderr, "relay %s: the drop notice is the wrong size\n",
                        node.own_name);
                if (reject(node.own_name, rcvid, EINVAL,
                           "rejecting the drop notice failed") != 0) {
                    return EXIT_FAILURE;
                }
                continue;
            }
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "relay %s: replying to a drop notice failed: %s\n",
                        node.own_name, strerror(errno));
                return EXIT_FAILURE;
            }
            announce_dropped(&node, buffer.dropped.number,
                             buffer.dropped.class_id);
            continue;
        }

        if (buffer.message.type == SIGNAL_MSG_BATCH) {
            if (info.msglen < (ssize_t)sizeof(struct signal_batch)) {
                fprintf(stderr, "relay %s: the batch is the wrong size\n",
                        node.own_name);
                if (reject(node.own_name, rcvid, EINVAL,
                           "rejecting the batch failed") != 0) {
                    return EXIT_FAILURE;
                }
                if (set_fifo(node.own_name, node.own_priority) != 0) {
                    return EXIT_FAILURE;
                }
                continue;
            }
            if (admit_batch(&node, &buffer.batch, rcvid) != 0) {
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.message.type != SIGNAL_MSG_TYPE ||
            info.msglen < (ssize_t)sizeof(struct signal_message)) {
            fprintf(stderr,
                    "relay %s: the message is not a signal (type %u, %lld bytes, "
                    "need type %u and %zu bytes)\n",
                    node.own_name, buffer.message.type, (long long)info.msglen,
                    SIGNAL_MSG_TYPE, sizeof(struct signal_message));
            if (reject(node.own_name, rcvid, EINVAL,
                       "rejecting that message failed") != 0) {
                return EXIT_FAILURE;
            }
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            continue;
        }

        if (signal_class_priority(buffer.message.body.class_id) < 0) {
            fprintf(stderr, "relay %s: rejecting signal %" PRIu64 " of unknown class %u\n",
                    node.own_name, buffer.message.body.number,
                    buffer.message.body.class_id);
            if (reject(node.own_name, rcvid, EINVAL,
                       "rejecting an unknown class failed") != 0) {
                return EXIT_FAILURE;
            }
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.message.body.hop_count >= SIGNAL_MAX_HOPS) {
            fprintf(stderr,
                    "relay %s: signal %" PRIu64 " already has %u hop stamps\n",
                    node.own_name, buffer.message.body.number,
                    buffer.message.body.hop_count);
            if (reject(node.own_name, rcvid, EMSGSIZE,
                       "rejecting a signal with no room for a hop stamp failed") != 0) {
                return EXIT_FAILURE;
            }
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            continue;
        }

        pthread_mutex_lock(&node.lock);
        full = signal_queue_count(&node.queue) >= node.capacity;
        /*
         * next_coid is -1 until the first name_open. That is not a
         * broken link: the burst has to sit in the queue. link_down is
         * set only after a connection that already existed has failed.
         */
        if (node.link_down) {
            pthread_mutex_unlock(&node.lock);
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "relay %s: replying to the sender failed: %s\n",
                        node.own_name, strerror(errno));
                return EXIT_FAILURE;
            }
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            write_lost(&node, &buffer.message.body);
            continue;
        }
        if (full &&
            buffer.message.body.class_id == SIGNAL_CLASS_BACKGROUND) {
            pthread_mutex_unlock(&node.lock);
            /* EOK: source stops the run on any other status (D-042). */
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "relay %s: replying to the sender failed: %s\n",
                        node.own_name, strerror(errno));
                return EXIT_FAILURE;
            }
            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            note_drop(&node, buffer.message.body.number,
                      buffer.message.body.class_id);
            continue;
        }
        if (full) {
            struct signal_queue_item evicted;
            int took;

            /*
             * BACKGROUND first. A COMMAND with nowhere to go is dropped.
             * An URGENT then takes the newest COMMAND. A queue of only
             * URGENT signals is still EAGAIN (D-044).
             */
            took = signal_queue_evict_newest(&node.queue,
                                             SIGNAL_CLASS_BACKGROUND,
                                             &evicted);
            if (took != 0 &&
                buffer.message.body.class_id == SIGNAL_CLASS_COMMAND) {
                pthread_mutex_unlock(&node.lock);
                /* EOK: source stops the run on any other status (D-044). */
                if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                    fprintf(stderr,
                            "relay %s: replying to the sender failed: %s\n",
                            node.own_name, strerror(errno));
                    return EXIT_FAILURE;
                }
                if (set_fifo(node.own_name, node.own_priority) != 0) {
                    return EXIT_FAILURE;
                }
                note_drop(&node, buffer.message.body.number,
                          buffer.message.body.class_id);
                continue;
            }
            if (took != 0) {
                took = signal_queue_evict_newest(&node.queue,
                                                 SIGNAL_CLASS_COMMAND,
                                                 &evicted);
            }
            if (took != 0) {
                pthread_mutex_unlock(&node.lock);
                fprintf(stderr,
                        "relay %s: queue is full, rejecting signal %" PRIu64 "\n",
                        node.own_name, buffer.message.body.number);
                if (reject(node.own_name, rcvid, EAGAIN,
                           "rejecting a signal because the queue is full failed") != 0) {
                    return EXIT_FAILURE;
                }
                if (set_fifo(node.own_name, node.own_priority) != 0) {
                    return EXIT_FAILURE;
                }
                continue;
            }
            pthread_mutex_unlock(&node.lock);
            if (evicted.body.class_id == SIGNAL_CLASS_URGENT) {
                fprintf(stderr,
                        "relay %s: eviction returned signal %" PRIu64
                        " class %s\n",
                        node.own_name, evicted.body.number,
                        signal_class_name(evicted.body.class_id));
                return EXIT_FAILURE;
            }
            note_drop(&node, evicted.body.number, evicted.body.class_id);
        } else {
            pthread_mutex_unlock(&node.lock);
        }

        now_ns = monotonic_ns(node.own_name);
        stamped = buffer.message.body;
        stamped.hop_ns[stamped.hop_count] = now_ns;
        stamped.hop_count++;

        /* Reply first, so the forward thread cannot preempt this receive
         * before the sender is released (D-031, D-035). Queue the signal
         * before dropping to the node priority (D-050).
         */
        if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
            fprintf(stderr, "relay %s: replying to the sender failed: %s\n",
                    node.own_name, strerror(errno));
            return EXIT_FAILURE;
        }
        {
            int queued = enqueue(&node, &stamped, now_ns);

            if (set_fifo(node.own_name, node.own_priority) != 0) {
                return EXIT_FAILURE;
            }
            if (queued > 0) {
                write_lost(&node, &stamped);
                continue;
            }
            if (queued != 0) {
                fprintf(stderr,
                        "relay %s: signal %" PRIu64 " was accepted but did not fit "
                        "in the queue\n",
                        node.own_name, stamped.number);
                return EXIT_FAILURE;
            }
        }
    }
}
