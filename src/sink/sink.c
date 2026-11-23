#define _QNX_SOURCE

#include "issued_log.h"
#include "net_state.h"
#include "signal_ledger.h"
#include "signal_msg.h"
#include "telemetry_pub.h"
#include "telemetry_snap.h"
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
_Static_assert(SIGNAL_MANIFEST_CAP == SIGNAL_LEDGER_CAP,
               "the manifest and the ledger cover different numbers");

#define SAFETY_SECONDS 45u
#define DEFAULT_GAP_MS 1000ul

static const struct signal_ledger *pub_ledger;
static enum net_state current_state = NET_INIT;
static struct telem_window hop_window[3];
static int telemetry_on;

static void class_counts(const struct signal_ledger *ledger, uint32_t id,
                         unsigned *on_time, unsigned *late, unsigned *lost,
                         unsigned *dropped)
{
    unsigned number;

    *on_time = 0;
    *late = 0;
    *lost = 0;
    *dropped = 0;
    for (number = 1; number <= SIGNAL_LEDGER_CAP; number++) {
        if (ledger->class_id[number] != id) {
            continue;
        }
        if (ledger->account[number] == SIGNAL_ACCOUNT_ON_TIME) {
            (*on_time)++;
        } else if (ledger->account[number] == SIGNAL_ACCOUNT_LATE) {
            (*late)++;
        } else if (ledger->account[number] == SIGNAL_ACCOUNT_LOST) {
            (*lost)++;
        } else if (ledger->account[number] == SIGNAL_ACCOUNT_DROPPED) {
            (*dropped)++;
        }
    }
}

static void publish_sink(const struct signal_ledger *ledger,
                         enum net_state state, int flush)
{
    struct telem_class klass[3];
    char buf[TELEM_REPORT_CAP];
    int i;
    int n;

    if (!telemetry_on || ledger == NULL) {
        return;
    }
    memset(klass, 0, sizeof klass);
    for (i = 0; i < 3; i++) {
        unsigned on_time;
        unsigned late;
        unsigned lost;
        unsigned dropped;

        class_counts(ledger, (uint32_t)(i + 1), &on_time, &late, &lost,
                     &dropped);
        klass[i].on_time = on_time;
        klass[i].late = late;
        klass[i].lost = lost;
        klass[i].dropped = dropped;
        telem_class_set_latency(&klass[i], &hop_window[i]);
    }
    n = telem_report_sink(buf, sizeof buf, state, klass);
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

struct receive_buffer {
    union {
        struct _pulse pulse;
        struct signal_message message;
        struct signal_manifest manifest;
        struct signal_lock lock;
        struct signal_dropped dropped;
    } body;
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

static int pin_cpu0(void)
{
    /* Bit 0 is CPU 0. The mask is the pointer value, not a pointer. */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)1) == -1) {
        fprintf(stderr, "sink: pinning this thread to CPU 0 failed: %s\n",
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

static int parse_count(const char *text, unsigned long *out, int allow_zero)
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
    if (!allow_zero && value == 0) {
        return -1;
    }
    *out = value;
    return 0;
}

static void show_state(enum net_state *shown, enum net_state now)
{
    current_state = now;
    if (*shown != now) {
        *shown = now;
        printf("sink: state %s\n", net_state_name(now));
        fflush(stdout);
    }
    if (pub_ledger != NULL) {
        publish_sink(pub_ledger, now, 0);
    }
}

static void print_signal(const struct signal *sig, enum signal_verdict verdict,
                         enum net_presentation presentation, int64_t now_ns)
{
    int64_t prev = sig->issued_ns;
    uint32_t i;
    uint32_t hops = sig->hop_count;

    if (hops > SIGNAL_MAX_HOPS) {
        hops = SIGNAL_MAX_HOPS;
    }

    printf("sink: signal %" PRIu64 " class %s %s",
           sig->number, signal_class_name(sig->class_id),
           signal_verdict_name(verdict));
    if (presentation == NET_PRESENT_FRESH) {
        printf(", fresh");
    } else if (verdict == SIGNAL_ON_TIME) {
        printf(", held");
    }
    printf(", hops");
    for (i = 0; i < hops; i++) {
        printf(" %" PRId64 " ns,", sig->hop_ns[i] - prev);
        prev = sig->hop_ns[i];
    }
    printf(" %" PRId64 " ns, total %" PRId64 " ns\n",
           now_ns - prev, now_ns - sig->issued_ns);
    fflush(stdout);
}

static void print_summary(const struct signal_ledger *ledger)
{
    static const uint32_t classes[] = {
        SIGNAL_CLASS_URGENT,
        SIGNAL_CLASS_COMMAND,
        SIGNAL_CLASS_BACKGROUND
    };
    size_t i;

    for (i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        uint32_t id = classes[i];
        unsigned received = 0;
        unsigned on_time = 0;
        unsigned late = 0;
        unsigned lost = 0;
        unsigned dropped = 0;

        class_counts(ledger, id, &on_time, &late, &lost, &dropped);
        received = on_time + late;
        printf("sink: summary %s received %u, on time %u, late %u, lost %u, "
               "dropped %u\n",
               signal_class_name(id), received, on_time, late, lost, dropped);
    }
    printf("sink: unaccounted %u\n", signal_ledger_unaccounted(ledger));
    fflush(stdout);
    publish_sink(ledger, current_state, 1);
}

static void print_lost(const struct signal_ledger *ledger, uint64_t number)
{
    printf("sink: lost signal %" PRIu64 " class %s\n",
           number, signal_class_name(ledger->class_id[number]));
    fflush(stdout);
}

static int close_books(struct signal_ledger *ledger)
{
    unsigned number;

    for (number = 1; number <= SIGNAL_LEDGER_CAP; number++) {
        if (!ledger->issued[number] ||
            ledger->account[number] != SIGNAL_ACCOUNT_NONE) {
            continue;
        }
        if (signal_ledger_lose(ledger, number, ledger->class_id[number]) != 0) {
            return -1;
        }
        print_lost(ledger, number);
    }
    return signal_ledger_unaccounted(ledger) == 0 ? 0 : -1;
}

static int arm_receive(uint64_t gap_ns)
{
    /*
     * TimerTimeout is one-shot and is cleared by the next kernel call,
     * so it sits immediately in front of MsgReceive.
     */
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &gap_ns,
                     NULL) == -1) {
        fprintf(stderr, "sink: arming the urgent-pulse timeout failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    name_attach_t *attach;
    static struct signal_ledger ledger;
    struct net_machine machine;
    static struct receive_buffer buffer;
    enum net_state shown = NET_LOCKED_SAFE;
    unsigned long expected;
    unsigned long gap_ms = DEFAULT_GAP_MS;
    int64_t gap_ns;
    int finished = 0;
    int manifest_seen = 0;
    /* Default pin is the D-049 result: free runs stalled, pinned runs did not. */
    int pin = 1;
    int arg0 = 1;

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        if (strcmp(argv[2], "0") != 0) {
            fprintf(stderr, "usage: sink [-c 0] <signal-count> [urgent-gap-ms]\n");
            return EXIT_FAILURE;
        }
        pin = 1;
        arg0 = 3;
    }
    if ((argc - arg0 != 1 && argc - arg0 != 2) ||
        parse_count(argv[arg0], &expected, 0) != 0 ||
        (argc - arg0 == 2 && parse_count(argv[arg0 + 1], &gap_ms, 0) != 0)) {
        fprintf(stderr, "usage: sink [-c 0] <signal-count> [urgent-gap-ms]\n");
        return EXIT_FAILURE;
    }
    if (expected > SIGNAL_LEDGER_CAP) {
        fprintf(stderr, "sink: the count must be 1..%d\n", SIGNAL_LEDGER_CAP);
        return EXIT_FAILURE;
    }
    /*
     * The count is only a limit check. The loop ends when the manifest
     * arrives. Numbers the relay marked as dropped are closed from that
     * manifest, so they do not have to show up as messages.
     */

    if (install_alarm() == -1) {
        return EXIT_FAILURE;
    }
    if (pin && pin_cpu0() != 0) {
        return EXIT_FAILURE;
    }

    signal_ledger_init(&ledger);
    gap_ns = (int64_t)gap_ms * 1000000LL;
    net_machine_init(&machine, monotonic_ns(), gap_ns, NET_RECOVER_URGENT_NEED);
    {
        int i;

        for (i = 0; i < 3; i++) {
            telem_window_init(&hop_window[i]);
        }
    }
    if (telem_pub_open("sink")) {
        telemetry_on = 1;
        pub_ledger = &ledger;
    }
    show_state(&shown, machine.state);

    attach = name_attach(NULL, SIGNAL_ATTACH_NAME, 0);
    if (attach == NULL) {
        fprintf(stderr, "sink: could not register the name %s: %s\n",
                SIGNAL_ATTACH_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    alarm(SAFETY_SECONDS);
    while (!finished) {
        struct _msg_info info;
        rcvid_t rcvid;
        enum signal_verdict verdict;
        enum net_presentation presentation = NET_PRESENT_HELD;
        int64_t now_ns;

        now_ns = monotonic_ns();
        if (now_ns > machine.next_urgent_due_ns) {
            show_state(&shown, net_machine_on_tick(&machine, now_ns));
        }
        if (arm_receive((uint64_t)gap_ns) != 0) {
            print_summary(&ledger);
            detach(attach);
            return EXIT_FAILURE;
        }

        memset(&buffer, 0, sizeof buffer);
        memset(&info, 0, sizeof info);
        rcvid = MsgReceive(attach->chid, &buffer, sizeof buffer, &info);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            if (errno == EINTR) {
                fprintf(stderr,
                        "sink: stopped, nothing closed the books for %u seconds\n",
                        SAFETY_SECONDS);
            } else {
                fprintf(stderr, "sink: waiting for a signal failed: %s\n",
                        strerror(errno));
            }
            print_summary(&ledger);
            detach(attach);
            return EXIT_FAILURE;
        }

        if (rcvid == 0) {
            if (buffer.body.pulse.code == _PULSE_CODE_DISCONNECT) {
                if (ConnectDetach(buffer.body.pulse.scoid) == -1) {
                    fprintf(stderr,
                            "sink: detaching a departed client failed: %s\n",
                            strerror(errno));
                }
            } else if (buffer.body.pulse.code == _PULSE_CODE_UNBLOCK) {
                if (MsgError(buffer.body.pulse.value.sival_int, EINTR) == -1) {
                    fprintf(stderr,
                            "sink: releasing a client that asked to unblock "
                            "failed: %s\n",
                            strerror(errno));
                }
            }
            continue;
        }

        if (info.msglen < (ssize_t)sizeof(uint16_t) ||
            buffer.body.message.type <= _IO_MAX) {
            if (reject(rcvid, ENOSYS, "rejecting a system message failed") == -1) {
                print_summary(&ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

        if (buffer.body.message.type == SIGNAL_MSG_DROPPED &&
            info.msglen >= (ssize_t)sizeof(struct signal_dropped)) {
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "sink: replying to a drop notice failed: %s\n",
                        strerror(errno));
                print_summary(&ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            show_state(&shown,
                       net_machine_on_dropped(&machine,
                                              buffer.body.dropped.class_id));
            continue;
        }

        if (buffer.body.message.type == SIGNAL_MSG_LOCK &&
            info.msglen >= (ssize_t)sizeof(struct signal_lock)) {
            show_state(&shown, net_machine_on_lock(&machine));
            if (!manifest_seen) {
                printf("sink: manifest did not arrive; this summary is only "
                       "signals sink saw\n");
            }
            print_summary(&ledger);
            fflush(stdout);
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "sink: replying to the lock failed: %s\n",
                        strerror(errno));
                detach(attach);
                return EXIT_FAILURE;
            }
            detach(attach);
            alarm(0);
            return EXIT_SUCCESS;
        }

        if (buffer.body.message.type == SIGNAL_MSG_MANIFEST &&
            info.msglen >= (ssize_t)sizeof(struct signal_manifest)) {
            unsigned item;

            if (buffer.body.manifest.count > SIGNAL_MANIFEST_CAP) {
                if (reject(rcvid, EINVAL, "rejecting the manifest failed") == -1) {
                    print_summary(&ledger);
                    detach(attach);
                    return EXIT_FAILURE;
                }
                continue;
            }
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "sink: replying to the manifest failed: %s\n",
                        strerror(errno));
                print_summary(&ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            for (item = 0; item < buffer.body.manifest.count; item++) {
                struct signal_manifest_item *one = &buffer.body.manifest.item[item];

                if (signal_ledger_issue(&ledger, one->number, one->class_id) != 0) {
                    fprintf(stderr,
                            "sink: manifest repeats signal %" PRIu64 " or uses a "
                            "number outside 1..%d\n",
                            one->number, SIGNAL_LEDGER_CAP);
                    print_summary(&ledger);
                    detach(attach);
                    return EXIT_FAILURE;
                }
            }
            manifest_seen = 1;
            if (issued_list_is_rebuilt(buffer.body.manifest.reserved)) {
                printf("sink: %s\n", issued_list_rebuilt_sentence());
                fflush(stdout);
            }
            {
                unsigned missed = signal_ledger_arrived_unissued(&ledger);

                if (missed > 0) {
                    printf("sink: issued list did not include %u arrivals\n",
                           missed);
                    fflush(stdout);
                }
            }
            for (item = 0; item < buffer.body.manifest.count; item++) {
                struct signal_manifest_item *one = &buffer.body.manifest.item[item];

                if (one->reserved != SIGNAL_MANIFEST_DROPPED) {
                    continue;
                }
                if (signal_ledger_drop(&ledger, one->number) != 0) {
                    fprintf(stderr,
                            "sink: could not record dropped signal %" PRIu64 "\n",
                            one->number);
                    print_summary(&ledger);
                    detach(attach);
                    return EXIT_FAILURE;
                }
                show_state(&shown,
                           net_machine_on_dropped(&machine, one->class_id));
            }
            if (close_books(&ledger) != 0) {
                fprintf(stderr, "sink: the manifest left a signal unaccounted\n");
                print_summary(&ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            finished = 1;
            continue;
        }

        if (buffer.body.message.type != SIGNAL_MSG_TYPE ||
            info.msglen < (ssize_t)sizeof(struct signal_message) ||
            buffer.body.message.body.hop_count > SIGNAL_MAX_HOPS) {
            fprintf(stderr,
                    "sink: the message is not a signal (type %u, %lld bytes, "
                    "hop count %u)\n",
                    buffer.body.message.type, (long long)info.msglen,
                    buffer.body.message.body.hop_count);
            if (reject(rcvid, EINVAL, "rejecting that message failed") == -1) {
                print_summary(&ledger);
                detach(attach);
                return EXIT_FAILURE;
            }
            continue;
        }

        now_ns = monotonic_ns();
        verdict = signal_check(&buffer.body.message.body, now_ns);
        if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
            fprintf(stderr, "sink: replying to the sender failed: %s\n",
                    strerror(errno));
            print_summary(&ledger);
            detach(attach);
            return EXIT_FAILURE;
        }

        if (signal_ledger_receive(&ledger, buffer.body.message.body.number,
                                  buffer.body.message.body.class_id,
                                  verdict) != 0) {
            fprintf(stderr, "sink: signal %" PRIu64 " was already accounted\n",
                    buffer.body.message.body.number);
            continue;
        }
        if (telemetry_on &&
            buffer.body.message.body.class_id >= SIGNAL_CLASS_URGENT &&
            buffer.body.message.body.class_id <= SIGNAL_CLASS_BACKGROUND) {
            int64_t latency = now_ns - buffer.body.message.body.issued_ns;

            if (latency < 0) {
                latency = 0;
            }
            telem_window_add(&hop_window[buffer.body.message.body.class_id - 1],
                             latency);
        }
        net_machine_on_signal(&machine, buffer.body.message.body.class_id,
                              verdict, now_ns, &presentation);
        print_signal(&buffer.body.message.body, verdict, presentation, now_ns);
        show_state(&shown, machine.state);
    }

    alarm(0);
    print_summary(&ledger);
    detach(attach);
    return EXIT_SUCCESS;
}
