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

/* Placeholder until a measured deadline is chosen. One local message fits. */
#define DEMO_DEADLINE_NS 1000000000LL
#define FIND_ATTEMPTS 40
#define FIND_PAUSE_NS 50000000L
#define SEND_TIMEOUT_SECONDS 5u

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
    message.body.deadline_ns = DEMO_DEADLINE_NS;

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

int main(int argc, char **argv)
{
    unsigned long urgent;
    unsigned long command;
    unsigned long background;
    unsigned long left_u;
    unsigned long left_c;
    unsigned long left_b;
    unsigned long total;
    uint64_t number;
    int coid;
    int ceiling;

    if (argc != 5 ||
        parse_count(argv[1], &urgent) != 0 ||
        parse_count(argv[2], &command) != 0 ||
        parse_count(argv[3], &background) != 0 ||
        argv[4][0] == '\0') {
        fprintf(stderr,
                "usage: source <urgent-count> <command-count> "
                "<background-count> <next-name>\n");
        return EXIT_FAILURE;
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
           ceiling, urgent, command, background, argv[4]);
    fflush(stdout);

    coid = open_next(argv[4]);
    if (coid == -1) {
        return EXIT_FAILURE;
    }

    left_u = urgent;
    left_c = command;
    left_b = background;
    number = 1;
    while (left_b > 0 || left_c > 0 || left_u > 0) {
        if (left_b > 0) {
            if (send_one(coid, number, SIGNAL_CLASS_BACKGROUND) != 0) {
                name_close(coid);
                return EXIT_FAILURE;
            }
            left_b--;
            number++;
        }
        if (left_c > 0) {
            if (send_one(coid, number, SIGNAL_CLASS_COMMAND) != 0) {
                name_close(coid);
                return EXIT_FAILURE;
            }
            left_c--;
            number++;
        }
        if (left_u > 0) {
            if (send_one(coid, number, SIGNAL_CLASS_URGENT) != 0) {
                name_close(coid);
                return EXIT_FAILURE;
            }
            left_u--;
            number++;
        }
    }

    /*
     * Drop below every class so the relay forward thread, still at its
     * own priority until it dequeues, can run and drain the queue.
     */
    if (set_fifo(SIGNAL_PRIO_OWN) != 0) {
        name_close(coid);
        return EXIT_FAILURE;
    }

    if (name_close(coid) == -1) {
        fprintf(stderr, "source: closing the connection to %s failed: %s\n",
                argv[4], strerror(errno));
        return EXIT_FAILURE;
    }

    printf("source: sent %lu signals\n", total);
    fflush(stdout);
    return EXIT_SUCCESS;
}
