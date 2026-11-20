#define _QNX_SOURCE

#include "signal_msg.h"
#include "signal_queue.h"
#include "signal_sched.h"
#include "wire.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
        if (errno != ENOENT) {
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

    for (;;) {
        struct signal_queue_item item;
        char waiting[4096];
        struct signal_message message;
        struct signal_reply reply;
        int priority;
        long sent;

        pthread_mutex_lock(&node->lock);
        while (signal_queue_count(&node->queue) == 0) {
            pthread_cond_wait(&node->ready, &node->lock);
        }
        if (signal_queue_pop(&node->queue, &item) != 0) {
            pthread_mutex_unlock(&node->lock);
            continue;
        }
        format_waiting(&node->queue, waiting, sizeof waiting);
        pthread_mutex_unlock(&node->lock);

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
        sent = MsgSend(coid, &message, sizeof message, &reply, sizeof reply);
        if (set_fifo(node->own_name, node->own_priority) != 0) {
            exit(EXIT_FAILURE);
        }
        if (sent == -1) {
            fprintf(stderr, "relay %s: sending signal %" PRIu64 " to %s failed: %s\n",
                    node->own_name, item.body.number, node->next_name,
                    strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (sent != EOK) {
            fprintf(stderr,
                    "relay %s: %s rejected signal %" PRIu64 " with status %ld\n",
                    node->own_name, node->next_name, item.body.number, sent);
            exit(EXIT_FAILURE);
        }

        printf("relay %s: sent signal %" PRIu64 " class %s; still waiting: %s\n",
               node->own_name, item.body.number,
               signal_class_name(item.body.class_id),
               waiting[0] == '\0' ? "none" : waiting);
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
    rc = signal_queue_push(&node->queue, sig, arrival_ns, node->next_order);
    if (rc == 0) {
        node->next_order++;
        pthread_cond_signal(&node->ready);
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

int main(int argc, char **argv)
{
    struct relay node;
    name_attach_t *attach;
    pthread_t forward;
    int ceiling;
    int create_rc;
    union {
        struct _pulse pulse;
        struct signal_message message;
    } buffer;

    if (argc != 3 || !name_ok(argv[1]) || !name_ok(argv[2])) {
        fprintf(stderr, "usage: relay <attach-name> <next-name>\n");
        return EXIT_FAILURE;
    }

    memset(&node, 0, sizeof node);
    node.own_name = argv[1];
    node.next_name = argv[2];
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

    create_rc = pthread_create(&forward, NULL, forward_main, &node);
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
        full = signal_queue_count(&node.queue) >= SIGNAL_QUEUE_CAP;
        pthread_mutex_unlock(&node.lock);
        if (full) {
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
        if (enqueue(&node, &stamped, now_ns) != 0) {
            fprintf(stderr,
                    "relay %s: signal %" PRIu64 " was accepted but did not fit "
                    "in the queue\n",
                    node.own_name, stamped.number);
            return EXIT_FAILURE;
        }
    }
}
