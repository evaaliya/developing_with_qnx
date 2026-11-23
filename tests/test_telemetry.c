#include "telemetry_snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_int(const char *name, int got, int want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %d, want %d)\n", name, got, want);
    failures++;
}

static void expect_i64(const char *name, int64_t got, int64_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %lld, want %lld)\n", name, (long long)got,
           (long long)want);
    failures++;
}

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %u, want %u)\n", name, got, want);
    failures++;
}

static void expect_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %llu, want %llu)\n", name,
           (unsigned long long)got, (unsigned long long)want);
    failures++;
}

static void expect_text(const char *name, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s\n got: %s\nwant: %s\n", name, got != NULL ? got : "(null)",
           want != NULL ? want : "(null)");
    failures++;
}

static void test_window(void)
{
    struct telem_window window;
    int64_t out = 99;
    uint32_t i;

    telem_window_init(&window);
    expect_int("empty median fails", telem_window_median(&window, &out), -1);
    expect_i64("empty median leaves the output", out, 99);
    expect_int("empty maximum fails", telem_window_max(&window, &out), -1);
    expect_int("null window add is ignored", 0, 0);
    telem_window_add(NULL, 1);
    expect_int("null median fails", telem_window_median(NULL, &out), -1);
    expect_int("null maximum output fails", telem_window_max(&window, NULL),
               -1);

    telem_window_add(&window, 10);
    expect_u32("one sample fills one slot", window.count, 1);
    expect_int("median of one sample", telem_window_median(&window, &out), 0);
    expect_i64("one sample is its own median", out, 10);
    expect_int("maximum of one sample", telem_window_max(&window, &out), 0);
    expect_i64("one sample is its own maximum", out, 10);

    telem_window_add(&window, 20);
    expect_int("even median", telem_window_median(&window, &out), 0);
    expect_i64("even median is the mean toward zero", out, 15);
    expect_int("maximum of two", telem_window_max(&window, &out), 0);
    expect_i64("maximum of two is the larger", out, 20);

    telem_window_init(&window);
    for (i = 1; i <= TELEM_WINDOW; i++) {
        telem_window_add(&window, (int64_t)i);
    }
    expect_u32("window reaches capacity", window.count, TELEM_WINDOW);
    telem_window_add(&window, (int64_t)TELEM_WINDOW + 1);
    expect_u32("a full window drops the oldest", window.count, TELEM_WINDOW);
    expect_int("median after the drop", telem_window_median(&window, &out), 0);
    /* Values left are 2..33. The two central ones are 17 and 18. */
    expect_i64("median is the middle of the surviving window", out, 17);
    expect_int("maximum after the drop", telem_window_max(&window, &out), 0);
    expect_i64("maximum is the newest sample", out, (int64_t)TELEM_WINDOW + 1);
    expect_int("the oldest sample is gone", window.value[0] == 1 ? 1 : 0, 0);
}

static void test_roundtrip(void)
{
    struct telem_snapshot snap;
    struct telem_snapshot back;
    struct telem_class kept;
    char buf[TELEM_REPORT_CAP];
    int n;
    const char *want =
        "seq 7\n"
        "time_ns 99\n"
        "state DEGRADED\n"
        "source_mode flood\n"
        "node source alive 1 restarts 0\n"
        "node relay-a alive 1 restarts 2\n"
        "node relay-b alive 0 restarts 1\n"
        "node sink alive 1 restarts 0\n"
        "class URGENT issued 3 on_time 2 late 1 lost 0 dropped 0 "
        "median_ns 15 max_ns 20\n"
        "class COMMAND issued 4 on_time 4 late 0 lost 0 dropped 0 "
        "median_ns none max_ns none\n"
        "class BACKGROUND issued 9 on_time 6 late 0 lost 0 dropped 3 "
        "median_ns 8 max_ns 8\n"
        "end\n";

    telem_snapshot_init(&snap);
    snap.seq = 7;
    snap.time_ns = 99;
    snap.state = NET_DEGRADED;
    snap.source_mode = TELEM_MODE_FLOOD;
    snap.node[TELEM_SOURCE].alive = 1;
    snap.node[TELEM_RELAY_A].alive = 1;
    snap.node[TELEM_RELAY_A].restarts = 2;
    snap.node[TELEM_RELAY_B].alive = 0;
    snap.node[TELEM_RELAY_B].restarts = 1;
    snap.node[TELEM_SINK].alive = 1;
    snap.klass[0].issued = 3;
    snap.klass[0].on_time = 2;
    snap.klass[0].late = 1;
    snap.klass[0].have_latency = 1;
    snap.klass[0].median_ns = 15;
    snap.klass[0].max_ns = 20;
    snap.klass[1].issued = 4;
    snap.klass[1].on_time = 4;
    snap.klass[2].issued = 9;
    snap.klass[2].on_time = 6;
    snap.klass[2].dropped = 3;
    snap.klass[2].have_latency = 1;
    snap.klass[2].median_ns = 8;
    snap.klass[2].max_ns = 8;

    n = telem_snapshot_format(&snap, buf, sizeof buf);
    expect_int("format writes the snapshot", n > 0, 1);
    expect_text("format text", buf, want);
    expect_int("parse accepts that text", telem_snapshot_parse(buf, &back), 0);
    expect_u64("parsed sequence", back.seq, 7);
    expect_i64("parsed time", back.time_ns, 99);
    expect_int("parsed state", (int)back.state, (int)NET_DEGRADED);
    expect_int("parsed mode", (int)back.source_mode, (int)TELEM_MODE_FLOOD);
    expect_int("parsed relay-b is dead", back.node[TELEM_RELAY_B].alive, 0);
    expect_u32("parsed relay-a restarts", back.node[TELEM_RELAY_A].restarts, 2);
    expect_u64("parsed urgent issued", back.klass[0].issued, 3);
    expect_u32("parsed urgent late", back.klass[0].late, 1);
    expect_i64("parsed urgent median", back.klass[0].median_ns, 15);
    expect_int("command has no latency", back.klass[1].have_latency, 0);
    expect_u32("parsed background dropped", back.klass[2].dropped, 3);

    kept = back.klass[0];
    expect_int("a short buffer is refused",
               telem_snapshot_format(&snap, buf, 8), -1);
    expect_int("a missing end is refused",
               telem_snapshot_parse("seq 1\ntime_ns 1\n", &back), -1);
    expect_u64("a refused parse leaves the snapshot", back.seq, 7);
    expect_int("the formatted snapshot parses again",
               telem_snapshot_parse(want, &back), 0);
    expect_int(
        "garbage state is refused",
        telem_snapshot_parse("seq 1\ntime_ns 1\nstate BOGUS\nsource_mode "
                             "normal\nnode source alive 1 restarts 0\nnode "
                             "relay-a alive 1 restarts 0\nnode relay-b alive 1 "
                             "restarts 0\nnode sink alive 1 restarts 0\nclass "
                             "URGENT issued 0 on_time 0 late 0 lost 0 dropped "
                             "0 median_ns none max_ns none\nclass COMMAND "
                             "issued 0 on_time 0 late 0 lost 0 dropped 0 "
                             "median_ns none max_ns none\nclass BACKGROUND "
                             "issued 0 on_time 0 late 0 lost 0 dropped 0 "
                             "median_ns none max_ns none\nend\n",
                             &back),
        -1);
    expect_u32("urgent on_time survived the refused parse", kept.on_time,
               back.klass[0].on_time);
    (void)kept;
}

static void test_merge(void)
{
    struct telem_snapshot snap;
    struct telem_class klass[3];
    struct telem_node node[TELEM_NODES];
    struct telem_window window;
    char source[TELEM_REPORT_CAP];
    char sink[TELEM_REPORT_CAP];
    char nodes[TELEM_REPORT_CAP];
    char again[TELEM_REPORT_CAP];
    int n;

    memset(klass, 0, sizeof klass);
    telem_window_init(&window);
    telem_window_add(&window, 4);
    telem_window_add(&window, 8);
    telem_class_set_latency(&klass[0], &window);
    klass[0].on_time = 2;
    telem_class_set_latency(&klass[1], &window);
    klass[1].late = 1;

    telem_snapshot_init(&snap);
    n = telem_report_source(source, sizeof source, TELEM_MODE_FLOOD, 3, 1, 8);
    expect_int("source report fits", n > 0, 1);
    expect_int("flood wins over mute",
               (int)telem_mode_from_flags(1, 0), (int)TELEM_MODE_FLOOD);
    expect_int("mute when urgent is off",
               (int)telem_mode_from_flags(0, 0), (int)TELEM_MODE_MUTE);
    expect_int("merge source report",
               telem_snapshot_merge_report(&snap, source), 0);
    expect_int("mode came from source", (int)snap.source_mode,
               (int)TELEM_MODE_FLOOD);
    expect_u64("issued urgent came from source", snap.klass[0].issued, 3);
    expect_int("state stays INIT until sink speaks", (int)snap.state,
               (int)NET_INIT);
    expect_int("nodes stay unknown", snap.node[TELEM_SINK].alive, -1);

    n = telem_report_sink(sink, sizeof sink, NET_DEGRADED, klass);
    expect_int("sink report fits", n > 0, 1);
    expect_int("merge sink report", telem_snapshot_merge_report(&snap, sink),
               0);
    expect_int("state came from sink", (int)snap.state, (int)NET_DEGRADED);
    expect_u32("on time came from sink", snap.klass[0].on_time, 2);
    expect_i64("median came from the window", snap.klass[0].median_ns, 6);
    expect_i64("maximum came from the window", snap.klass[0].max_ns, 8);
    expect_int("mode survived the sink report", (int)snap.source_mode,
               (int)TELEM_MODE_FLOOD);
    expect_u64("issued survived the sink report", snap.klass[0].issued, 3);

    memset(node, 0, sizeof node);
    node[TELEM_SOURCE].alive = 1;
    node[TELEM_RELAY_A].alive = 1;
    node[TELEM_RELAY_A].restarts = 2;
    node[TELEM_RELAY_B].alive = 0;
    node[TELEM_RELAY_B].restarts = 1;
    node[TELEM_SINK].alive = 1;
    n = telem_report_nodes(nodes, sizeof nodes, node);
    expect_int("nodes report fits", n > 0, 1);
    expect_int("merge nodes report",
               telem_snapshot_merge_report(&snap, nodes), 0);
    expect_int("relay-b is dead", snap.node[TELEM_RELAY_B].alive, 0);
    expect_u32("relay-a restarts", snap.node[TELEM_RELAY_A].restarts, 2);
    expect_u32("urgent on time survived the nodes report",
               snap.klass[0].on_time, 2);

    n = telem_snapshot_format(&snap, again, sizeof again);
    expect_int("merged snapshot formats", n > 0, 1);
    expect_int("a broken report is refused",
               telem_snapshot_merge_report(&snap, "kind sink\nend\n"), -1);
    expect_int("state survived the broken report", (int)snap.state,
               (int)NET_DEGRADED);
    expect_int("null report is refused",
               telem_snapshot_merge_report(&snap, NULL), -1);
}

int main(void)
{
    test_window();
    test_roundtrip();
    test_merge();
    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
