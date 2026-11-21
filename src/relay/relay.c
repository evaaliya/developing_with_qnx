#define _QNX_SOURCE

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

#define FIND_ATTEMPTS 40
#define FIND_PAUSE_NS 50000000L
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
    int next_coid;
    int link_down;
    int manifest_waiting;
    rcvid_t manifest_rcvid;
    struct signal_manifest manifest;
    size_t capacity;
    uint64_t dropped[SIGNAL_MANIFEST_CAP];
    unsigned dropped_count;
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
    printf("relay %s: dropped signal %" PRIu64 " class %s\n",
           node->own_name, number, signal_class_name(class_id));
    fflush(stdout);
    if (node->dropped_count >= SIGNAL_MANIFEST_CAP) {
        fprintf(stderr,
                "relay %s: the dropped list is full, signal %" PRIu64
                " will not be marked on the manifest\n",
                node->own_name, number);
        return;
    }
    node->dropped[node->dropped_count++] = number;
}

static void mark_dropped(struct relay *node, struct signal_manifest *manifest)
{
    uint16_t i;
    uint16_t count = manifest->count;

    if (count > SIGNAL_MANIFEST_CAP) {
        count = SIGNAL_MANIFEST_CAP;
    }
    for (i = 0; i < count; i++) {
        unsigned d;

        for (d = 0; d < node->dropped_count; d++) {
            if (manifest->item[i].number != node->dropped[d]) {
                continue;
            }
            manifest->item[i].reserved = SIGNAL_MANIFEST_DROPPED;
            break;
        }
    }
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
        struct signal_manifest manifest;
        char waiting[4096];
        struct signal_message message;
        struct signal_reply reply;
        int have_item = 0;
        int have_manifest = 0;
        rcvid_t manifest_rcvid = 0;
        int priority;
        long sent;
        int64_t arrived_ns = 0;
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
         */
        if (item.body.hop_count >= 1 &&
            item.body.hop_count <= SIGNAL_MAX_HOPS) {
            arrived_ns = item.body.hop_ns[item.body.hop_count - 1];
        } else {
            arrived_ns = 0;
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

        printf("relay %s: sent signal %" PRIu64
               " class %s; still waiting: %s arrived %" PRId64
               " start %" PRId64 " end %" PRId64 "\n",
               node->own_name, item.body.number,
               signal_class_name(item.body.class_id),
               waiting[0] == '\0' ? "none" : waiting,
               arrived_ns, start_ns, end_ns);
        fflush(stdout);
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
                            const char **next, size_t *capacity)
{
    const char *names[2];
    int name_count = 0;
    int got_q = 0;
    unsigned long value = SIGNAL_QUEUE_CAP;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0) {
            if (got_q || i + 1 >= argc) {
                fprintf(stderr,
                        "usage: relay <attach-name> <next-name> [-q N]\n");
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
            fprintf(stderr, "usage: relay <attach-name> <next-name> [-q N]\n");
            return -1;
        }
        names[name_count++] = argv[i];
    }
    if (name_count != 2) {
        fprintf(stderr, "usage: relay <attach-name> <next-name> [-q N]\n");
        return -1;
    }
    *own = names[0];
    *next = names[1];
    *capacity = (size_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct relay node;
    name_attach_t *attach;
    int ceiling;
    int create_rc;
    union {
        struct _pulse pulse;
        struct signal_message message;
        struct signal_manifest manifest;
    } buffer;

    const char *own_name;
    const char *next_name;
    size_t capacity;

    if (parse_relay_args(argc, argv, &own_name, &next_name, &capacity) != 0) {
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
    node.next_order = 1;

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

        /* Reply before the forward thread can see the signal (D-031, D-035). */
        if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
            fprintf(stderr, "relay %s: replying to the sender failed: %s\n",
                    node.own_name, strerror(errno));
            return EXIT_FAILURE;
        }
        if (set_fifo(node.own_name, node.own_priority) != 0) {
            return EXIT_FAILURE;
        }
        {
            int queued = enqueue(&node, &stamped, now_ns);

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
