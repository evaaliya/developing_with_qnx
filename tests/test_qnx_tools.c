/*
 * Host tests for the "QNX tools" network (D-098): argument parsing, the
 * planned event time, and the log lines the reports read, held against
 * lines of the Nerve network. Lines from the real B29 log are read from
 * the file given as the first argument (replay/b29/ours-flood-11.txt).
 * The second argument is where the sink lines for latency-report go.
 *   test-qnx-tools <nerve-log> <out-lines>
 */
#include "arm_plan.h"
#include "qt_core.h"
#include "signal_msg.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass;
static int g_fail;

static void check(int ok, const char *what)
{
    if (ok) {
        g_pass++;
        printf("PASS %s\n", what);
    } else {
        g_fail++;
        printf("FAIL %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    int ok = got != NULL && strcmp(got, want) == 0;

    check(ok, what);
    if (!ok) {
        printf("  want: %s  got:  %s\n", want, got == NULL ? "(null)\n" : got);
    }
}

static int argn(char **v)
{
    int n = 0;

    while (v[n] != NULL) {
        n++;
    }
    return n;
}

static void test_args(void)
{
    struct qt_opts o;
    const char *a;
    const char *b;
    unsigned long count = 0;
    unsigned long gap = 0;
    char *s1[] = { "source", "-o", "/tmp/source.out", "-q", "live",
                   "signal-relay-a", NULL };
    char *s2[] = { "source", "live", "signal-relay-a", NULL };
    char *s3[] = { "source", "live", NULL };
    char *s4[] = { "source", "-o", NULL };
    char *s5[] = { "source", "live", "-r", "signal-relay-a", NULL };
    char *r1[] = { "relay", "signal-relay-a", "signal-relay-b", NULL };
    char *r2[] = { "relay", "-o", "/tmp/relay-a.out", "signal-relay-a",
                   "signal-relay-b", "-q", "8", "-r", NULL };
    char *r3[] = { "relay", "signal-relay-a", NULL };
    char *r4[] = { "relay", "signal-relay-a", "signal-relay-b", "-x", NULL };
    char *k1[] = { "sink", "64", "2000", NULL };
    char *k2[] = { "sink", "64", NULL };
    char *k3[] = { "sink", "-o", "/tmp/sink.out", "-q", "-c", "0", "-r", "64",
                   "2000", NULL };
    char *k4[] = { "sink", "0", NULL };
    char *k5[] = { "sink", "-c", "1", "64", NULL };
    char *k6[] = { "sink", "64", "2000", "3", NULL };

    check(qt_parse_opts(argn(s1), s1, &o) == 4 && o.quiet == 1 &&
          strcmp(o.out, "/tmp/source.out") == 0, "opts -o FILE -q");
    check(qt_parse_source(argn(s1), s1, 4, &a) == 0 &&
          strcmp(a, "signal-relay-a") == 0, "source live after options");
    check(qt_parse_opts(argn(s2), s2, &o) == 1 && o.out == NULL && !o.quiet,
          "opts none");
    check(qt_parse_source(argn(s2), s2, 1, &a) == 0, "source live <next>");
    check(qt_parse_source(argn(s3), s3, 1, &a) == -1, "source without next");
    check(qt_parse_opts(argn(s4), s4, &o) == -1, "opts -o without a file");
    check(qt_parse_source(argn(s5), s5, 1, &a) == -1,
          "source -r is not taken (no books, D-098)");
    check(qt_parse_relay(argn(r1), r1, 1, &a, &b) == 0 &&
          strcmp(a, "signal-relay-a") == 0 && strcmp(b, "signal-relay-b") == 0,
          "relay <own> <next>");
    check(qt_parse_opts(argn(r2), r2, &o) == 3 &&
          qt_parse_relay(argn(r2), r2, 3, &a, &b) == 0,
          "relay accepts the Nerve -q N -r");
    check(qt_parse_relay(argn(r3), r3, 1, &a, &b) == -1, "relay without next");
    check(qt_parse_relay(argn(r4), r4, 1, &a, &b) == -1, "relay unknown flag");
    check(qt_parse_sink(argn(k1), k1, 1, &count, &gap) == 0 && count == 64 &&
          gap == 2000, "sink 64 2000: window 2000 ms");
    check(qt_parse_sink(argn(k2), k2, 1, &count, &gap) == 0 && gap == 1000,
          "sink default window 1000 ms (as the Nerve sink)");
    check(qt_parse_opts(argn(k3), k3, &o) == 4 &&
          qt_parse_sink(argn(k3), k3, 4, &count, &gap) == 0 && gap == 2000,
          "sink -c 0 -r after options");
    check(qt_parse_sink(argn(k4), k4, 1, &count, &gap) == -1, "sink count 0");
    check(qt_parse_sink(argn(k5), k5, 1, &count, &gap) == -1, "sink -c 1");
    check(qt_parse_sink(argn(k6), k6, 1, &count, &gap) == -1, "sink extra arg");
}

static void test_limits(void)
{
    check(qt_send_limit_ns(SIGNAL_CLASS_URGENT) == 31000000LL, "URGENT limit 31 ms");
    check(qt_send_limit_ns(SIGNAL_CLASS_COMMAND) == 62000000LL, "COMMAND limit 62 ms");
    check(qt_send_limit_ns(SIGNAL_CLASS_BACKGROUND) == 124000000LL,
          "BACKGROUND limit 124 ms");
    check(qt_send_limit_ns(0) == -1, "no limit for an unknown class");
}

static void test_lines(void)
{
    char buf[256];
    struct signal sig;

    qt_line_send_timeout(buf, sizeof buf, SIGNAL_CLASS_URGENT, 2835, 31004000);
    check_str(buf, "source: send-timeout class URGENT seq 2835 after_ns 31004000\n",
              "send-timeout line");
    qt_line_send_failed(buf, sizeof buf, SIGNAL_CLASS_BACKGROUND, 9, ESRCH);
    {
        char want[96];

        snprintf(want, sizeof want,
                 "source: send-failed class BACKGROUND seq 9 errno %d\n", ESRCH);
        check_str(buf, want, "send-failed line");
    }
    check(qt_line_send_timeout(buf, 10, SIGNAL_CLASS_URGENT, 1, 1) == -1,
          "short buffer is refused");

    /* The Nerve fixture line "hops 1 ns, 2 ns, 1000 ns, total 1003 ns". */
    memset(&sig, 0, sizeof sig);
    sig.number = 1;
    sig.class_id = SIGNAL_CLASS_URGENT;
    sig.issued_ns = 0;
    sig.deadline_ns = SIGNAL_DEADLINE_URGENT_NS;
    sig.hop_count = 2;
    sig.hop_ns[0] = 1;
    sig.hop_ns[1] = 3;
    qt_line_sink_signal(buf, sizeof buf, &sig, SIGNAL_ON_TIME, 1003);
    check_str(buf, "sink: signal 1 class URGENT on time, hops 1 ns, 2 ns, 1000 ns, "
              "total 1003 ns\n", "sink signal line as the Nerve fixture");

    /* A Nerve late line from the B28 arm series. */
    sig.number = 15;
    sig.issued_ns = 1067966000000LL - 40151008LL + 500000LL;
    qt_line_sink_late(buf, sizeof buf, &sig, sig.issued_ns + 40151008LL);
    check_str(buf, "sink: late signal 15 class URGENT at 1067966 ms, total "
              "40151008 ns\n", "sink late line as Nerve");
    qt_line_sink_state(buf, sizeof buf, "SAFE", 0, 0);
    check_str(buf, "sink: state SAFE\n", "sink state line, not quiet");
}

/* Lines rebuilt from the numbers of the real Nerve log must match it. */
static void test_nerve_log(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[2048];
    char buf[512];
    int plan = 0;
    int event = 0;
    int state = 0;

    if (f == NULL) {
        check(0, "open the Nerve log");
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        long long a, b, c, d, e;
        long eye;
        unsigned long long n;

        if (sscanf(line, "source: person-plan approach_start_ns %lld start_eye_um "
                   "%ld offset_ns %lld event_ns %lld woke_late_ns %lld", &a, &eye,
                   &b, &c, &d) == 5) {
            qt_line_person_plan(buf, sizeof buf, a, (int32_t)eye, b, c, c + d,
                                ARM_PLAN_MARGIN_M, arm_plan_event_distance_m());
            check_str(buf, line, "person-plan line as the Nerve log");
            check(b == arm_plan_time_to_distance_ns(arm_plan_event_distance_m()),
                  "planned offset is the one in the Nerve log (D-089)");
            plan = 1;
        } else if (sscanf(line, "source: person-in event_ns %lld URGENT %llu",
                          &a, &n) == 2) {
            qt_line_person_event(buf, sizeof buf, a, (uint64_t)n);
            check_str(buf, line, "person-in event line as the Nerve log");
            event = 1;
        } else if (!state && sscanf(line, "sink: state NORMAL at %lld ms", &e) == 1) {
            qt_line_sink_state(buf, sizeof buf, "NORMAL", 1, e);
            check_str(buf, line, "sink state line as the Nerve log");
            state = 1;
        }
    }
    fclose(f);
    check(plan && event && state, "the Nerve log has the three lines");
}

/* Sink lines for latency-report, which must read every one of them. */
static void write_report_lines(const char *path)
{
    FILE *f = fopen(path, "w");
    char buf[256];
    struct signal sig;
    unsigned i;

    if (f == NULL) {
        check(0, "write the lines for latency-report");
        return;
    }
    for (i = 1; i <= 6; i++) {
        memset(&sig, 0, sizeof sig);
        sig.number = i;
        sig.class_id = 1 + (i % 3);
        sig.issued_ns = 1000000;
        sig.deadline_ns = signal_class_deadline_ns(sig.class_id);
        sig.hop_count = 2;
        sig.hop_ns[0] = 1000000 + 100000 * i;
        sig.hop_ns[1] = 1000000 + 250000 * i;
        qt_line_sink_signal(buf, sizeof buf, &sig, SIGNAL_ON_TIME,
                            1000000 + 400000 * i);
        fputs(buf, f);
    }
    fclose(f);
    check(1, "lines for latency-report written");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: test-qnx-tools <nerve-log> <out-lines>\n");
        return 2;
    }
    test_args();
    test_limits();
    test_lines();
    test_nerve_log(argv[1]);
    write_report_lines(argv[2]);
    printf("qnx-tools tests: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
