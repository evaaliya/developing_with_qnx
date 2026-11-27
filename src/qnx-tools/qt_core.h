#ifndef QT_CORE_H
#define QT_CORE_H

/*
 * The "QNX tools" network (D-098): source -> relay A -> relay B -> sink
 * built from what QNX 8.0 already offers (name_attach, MsgSend, a thread
 * pool, TimerTimeout, HAM), without the Nerve logic. This file holds the
 * parts with no QNX header and no clock, so the host tests run them:
 * argument parsing and the log lines the reports read. The lines keep
 * the Nerve format byte for byte where a report reads them.
 */

#include "signal_msg.h"

#include <stddef.h>
#include <stdint.h>

/* The build name in the start lines; \w+ so the reports can read it. */
#define QT_BUILD_NAME "qnx_tools"

/*
 * The end of a run. Nerve ends a run with its manifest (accounting); this
 * network has none, so source sends this through the chain and the sink
 * prints its lines and exits. Above SIGNAL_MSG_TYPE and its siblings.
 */
#define QT_MSG_END ((uint16_t)530)

struct qt_end {
    uint16_t type;
    uint16_t reserved0;
    uint32_t reserved1;
};

/*
 * Options every node takes before the Nerve arguments, because HAM starts
 * the node with a plain command line (no shell): -o FILE appends stdout
 * and stderr to FILE, -q sets SIGNAL_QUIET=1 (no line per signal).
 */
struct qt_opts {
    const char *out;
    int quiet;
};

/* Index of the first argument after the options, or -1 on a bad option. */
int qt_parse_opts(int argc, char *const argv[], struct qt_opts *opts);

/* source: "live <next-name>" from index first. 0, or -1. */
int qt_parse_source(int argc, char *const argv[], int first,
                    const char **next_name);

/*
 * relay: "<attach-name> <next-name> [-q N] [-r]" from index first. The
 * Nerve queue size and resume flag are accepted and have no effect: this
 * relay has no queue of its own and no books (D-098 (3), (6)). 0, or -1.
 */
int qt_parse_relay(int argc, char *const argv[], int first,
                   const char **own_name, const char **next_name);

/*
 * sink: "[-c 0] [-r] <signal-count> [urgent-gap-ms]" from index first,
 * as the Nerve sink. The gap is the watchdog window; 1000 ms by default.
 * 0, or -1.
 */
#define QT_SINK_DEFAULT_GAP_MS 1000ul
int qt_parse_sink(int argc, char *const argv[], int first,
                  unsigned long *count, unsigned long *gap_ms);

/* The send time limit of a class: its deadline (D-058, D-098 (8)). -1 if none. */
int64_t qt_send_limit_ns(uint32_t class_id);

/*
 * Log lines. Each returns the bytes written without the NUL, or -1 when
 * the buffer is too small. Every line ends in a newline.
 */
int qt_line_send_timeout(char *buf, size_t cap, uint32_t class_id,
                         uint64_t number, int64_t after_ns);
int qt_line_send_failed(char *buf, size_t cap, uint32_t class_id,
                        uint64_t number, int err);
/* "source: person-plan approach_start_ns ..." as the Nerve source. */
int qt_line_person_plan(char *buf, size_t cap, int64_t start_ns,
                        int32_t start_eye_um, int64_t offset_ns,
                        int64_t event_ns, int64_t woke_ns,
                        double margin_m, double event_m);
int qt_line_person_event(char *buf, size_t cap, int64_t event_ns,
                         uint64_t number);
/* "sink: state NAME" or, quiet, "sink: state NAME at MS ms". */
int qt_line_sink_state(char *buf, size_t cap, const char *state, int quiet,
                       long long run_ms);
/* "sink: signal N class C on time|late, hops ... ns, total T ns". */
int qt_line_sink_signal(char *buf, size_t cap, const struct signal *sig,
                        enum signal_verdict verdict, int64_t now_ns);
int qt_line_sink_late(char *buf, size_t cap, const struct signal *sig,
                      int64_t now_ns);

#endif
