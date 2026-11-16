#define _QNX_SOURCE

#include "signal_msg.h"
#include "wire.h"

#include <errno.h>
#include <inttypes.h>
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
#define DEMO_NUMBER 1ULL
#define DEMO_CLASS SIGNAL_CLASS_COMMAND
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

static int open_sink(void)
{
    int attempt;

    for (attempt = 0; attempt < FIND_ATTEMPTS; attempt++) {
        int coid = name_open(SIGNAL_ATTACH_NAME, 0);
        struct timespec pause;

        if (coid != -1) {
            return coid;
        }
        if (errno != ENOENT) {
            fprintf(stderr, "source: opening the sink name %s failed: %s\n",
                    SIGNAL_ATTACH_NAME, strerror(errno));
            return -1;
        }
        pause.tv_sec = 0;
        pause.tv_nsec = FIND_PAUSE_NS;
        nanosleep(&pause, NULL);
    }

    fprintf(stderr,
            "source: could not find a sink registered as %s after 2 seconds\n",
            SIGNAL_ATTACH_NAME);
    return -1;
}

int main(void)
{
    struct signal_message message;
    struct signal_reply reply;
    int coid;
    long sent;

    if (install_alarm() == -1) {
        return EXIT_FAILURE;
    }

    coid = open_sink();
    if (coid == -1) {
        return EXIT_FAILURE;
    }

    memset(&message, 0, sizeof message);
    message.type = SIGNAL_MSG_TYPE;
    message.body.number = DEMO_NUMBER;
    message.body.class_id = DEMO_CLASS;
    message.body.issued_ns = monotonic_ns();
    message.body.deadline_ns = DEMO_DEADLINE_NS;

    memset(&reply, 0, sizeof reply);
    alarm(SEND_TIMEOUT_SECONDS);
    sent = MsgSend(coid, &message, sizeof message, &reply, sizeof reply);
    alarm(0);
    if (sent == -1) {
        fprintf(stderr, "source: sending the signal failed: %s\n",
                strerror(errno));
        name_close(coid);
        return EXIT_FAILURE;
    }
    if (sent != EOK) {
        fprintf(stderr, "source: the sink rejected the signal with status %ld\n",
                sent);
        name_close(coid);
        return EXIT_FAILURE;
    }

    if (name_close(coid) == -1) {
        fprintf(stderr, "source: closing the connection to the sink failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    printf("source: sent signal %" PRIu64 " class %s\n",
           (uint64_t)DEMO_NUMBER, signal_class_name(DEMO_CLASS));
    return EXIT_SUCCESS;
}
