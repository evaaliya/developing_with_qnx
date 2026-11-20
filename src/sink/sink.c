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

struct class_tally {
    unsigned long received;
    unsigned long on_time;
    unsigned long late;
};

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

static int parse_count(const char *text, unsigned long *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0 || value == 0) {
        return -1;
    }
    *out = value;
    return 0;
}

static void print_signal(const struct signal *sig, enum signal_verdict verdict,
                         int64_t now_ns)
{
    int64_t prev = sig->issued_ns;
    uint32_t i;
    uint32_t hops = sig->hop_count;

    if (hops > SIGNAL_MAX_HOPS) {
        hops = SIGNAL_MAX_HOPS;
    }

    printf("sink: signal %" PRIu64 " class %s %s, hops",
           sig->number, signal_class_name(sig->class_id),
           signal_verdict_name(verdict));
    for (i = 0; i < hops; i++) {
        printf(" %" PRId64 " ns,", sig->hop_ns[i] - prev);
        prev = sig->hop_ns[i];
    }
    printf(" %" PRId64 " ns, total %" PRId64 " ns\n",
           now_ns - prev, now_ns - sig->issued_ns);
    fflush(stdout);
}

static void print_summary(const struct class_tally *tally, unsigned long unknown)
{
    static const uint32_t classes[] = {
        SIGNAL_CLASS_URGENT,
        SIGNAL_CLASS_COMMAND,
        SIGNAL_CLASS_BACKGROUND
    };
    size_t i;

    for (i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        uint32_t id = classes[i];
        printf("sink: summary %s received %lu, on time %lu, late %lu\n",
               signal_class_name(id), tally[id].received, tally[id].on_time,
               tally[id].late);
    }
    if (unknown > 0) {
        printf("sink: summary unknown received %lu\n", unknown);
    }
    fflush(stdout);
}

static void add_tally(struct class_tally *tally, unsigned long *unknown,
                      uint32_t class_id, enum signal_verdict verdict)
{
    if (class_id != SIGNAL_CLASS_URGENT &&
        class_id != SIGNAL_CLASS_COMMAND &&
        class_id != SIGNAL_CLASS_BACKGROUND) {
        (*unknown)++;
        return;
    }
    tally[class_id].received++;
    if (verdict == SIGNAL_ON_TIME) {
        tally[class_id].on_time++;
    } else {
        tally[class_id].late++;
    }
}

int main(int argc, char **argv)
{
    name_attach_t *attach;
    struct class_tally tally[4];
    unsigned long expected;
    unsigned long received = 0;
    unsigned long unknown = 0;
    union {
        struct _pulse pulse;
        struct signal_message message;
    } buffer;

    if (argc != 2 || parse_count(argv[1], &expected) != 0) {
        fprintf(stderr, "usage: sink <signal-count>\n");
        return EXIT_FAILURE;
    }

    if (install_alarm() == -1) {
        return EXIT_FAILURE;
    }

    memset(tally, 0, sizeof tally);

    attach = name_attach(NULL, SIGNAL_ATTACH_NAME, 0);
    if (attach == NULL) {
        fprintf(stderr, "sink: could not register the name %s: %s\n",
                SIGNAL_ATTACH_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    alarm(RECEIVE_TIMEOUT_SECONDS);
    while (received < expected) {
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
                fprintf(stderr,
                        "sink: stopped after %lu of %lu signals, nothing "
                        "arrived for %u seconds\n",
                        received, expected, RECEIVE_TIMEOUT_SECONDS);
            } else {
                fprintf(stderr, "sink: waiting for a signal failed: %s\n",
                        strerror(errno));
            }
            print_summary(tally, unknown);
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
                print_summary(tally, unknown);
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.message.type != SIGNAL_MSG_TYPE ||
            info.msglen < (ssize_t)sizeof(struct signal_message) ||
            buffer.message.body.hop_count > SIGNAL_MAX_HOPS) {
            fprintf(stderr,
                    "sink: the message is not a signal (type %u, %lld bytes, "
                    "hop count %u)\n",
                    buffer.message.type, (long long)info.msglen,
                    buffer.message.body.hop_count);
            if (reject(rcvid, EINVAL, "rejecting that message failed") == -1) {
                print_summary(tally, unknown);
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

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
            print_summary(tally, unknown);
            detach(attach);
            return EXIT_FAILURE;
        }

        print_signal(&buffer.message.body, verdict, now_ns);
        add_tally(tally, &unknown, buffer.message.body.class_id, verdict);
        received++;
        alarm(RECEIVE_TIMEOUT_SECONDS);
    }

    alarm(0);
    print_summary(tally, unknown);
    detach(attach);
    return EXIT_SUCCESS;
}
