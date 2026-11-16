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

#define RECEIVE_TIMEOUT_SECONDS 5u

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

int main(void)
{
    name_attach_t *attach;
    union {
        struct _pulse pulse;
        struct signal_message message;
    } buffer;

    if (install_alarm() == -1) {
        return EXIT_FAILURE;
    }

    attach = name_attach(NULL, SIGNAL_ATTACH_NAME, 0);
    if (attach == NULL) {
        fprintf(stderr, "sink: could not register the name %s: %s\n",
                SIGNAL_ATTACH_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    alarm(RECEIVE_TIMEOUT_SECONDS);
    for (;;) {
        struct _msg_info info;
        rcvid_t rcvid;
        enum signal_verdict verdict;
        struct signal_reply reply;
        int64_t now_ns;
        int64_t transit_ns;

        memset(&buffer, 0, sizeof buffer);
        memset(&info, 0, sizeof info);
        rcvid = MsgReceive(attach->chid, &buffer, sizeof buffer, &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                fprintf(stderr, "sink: no signal arrived within %u seconds\n",
                        RECEIVE_TIMEOUT_SECONDS);
            } else {
                fprintf(stderr, "sink: waiting for a signal failed: %s\n",
                        strerror(errno));
            }
            detach(attach);
            return EXIT_FAILURE;
        }

        if (rcvid == 0) {
            if (buffer.pulse.code == _PULSE_CODE_DISCONNECT) {
                if (ConnectDetach(buffer.pulse.scoid) == -1) {
                    fprintf(stderr,
                            "sink: detaching a departed client failed: %s\n",
                            strerror(errno));
                }
            } else if (buffer.pulse.code == _PULSE_CODE_UNBLOCK) {
                if (MsgError(buffer.pulse.value.sival_int, EINTR) == -1) {
                    fprintf(stderr,
                            "sink: releasing a client that asked to unblock "
                            "failed: %s\n",
                            strerror(errno));
                }
            }
            continue;
        }

        if (info.msglen < (ssize_t)sizeof(uint16_t) ||
            buffer.message.type <= _IO_MAX) {
            if (reject(rcvid, ENOSYS, "rejecting a system message failed") == -1) {
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.message.type != SIGNAL_MSG_TYPE ||
            info.msglen < (ssize_t)sizeof(struct signal_message)) {
            fprintf(stderr,
                    "sink: the message is not a signal (type %u, %lld bytes, "
                    "need type %u and %zu bytes)\n",
                    buffer.message.type, (long long)info.msglen,
                    SIGNAL_MSG_TYPE, sizeof(struct signal_message));
            if (reject(rcvid, EINVAL, "rejecting that message failed") == -1) {
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

        alarm(0);
        now_ns = monotonic_ns();
        verdict = signal_check(&buffer.message.body, now_ns);
        transit_ns = now_ns - buffer.message.body.issued_ns;

        memset(&reply, 0, sizeof reply);
        reply.verdict = (int32_t)verdict;
        reply.received_ns = now_ns;
        reply.transit_ns = transit_ns;
        if (MsgReply(rcvid, EOK, &reply, sizeof reply) == -1) {
            fprintf(stderr, "sink: replying to the sender failed: %s\n",
                    strerror(errno));
            detach(attach);
            return EXIT_FAILURE;
        }

        printf("sink: signal %" PRIu64 " class %s %s, transit %" PRId64 " ns\n",
               buffer.message.body.number,
               signal_class_name(buffer.message.body.class_id),
               signal_verdict_name(verdict), transit_ns);
        fflush(stdout);
        detach(attach);
        return EXIT_SUCCESS;
    }
}
