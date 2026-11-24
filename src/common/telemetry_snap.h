#ifndef TELEMETRY_SNAP_H
#define TELEMETRY_SNAP_H

/*
 * Snapshot text for the host panel. No QNX headers and no clock:
 * the caller passes every number (D-054). A lost report is replaced
 * by the next one, so each report carries totals, not a delta.
 *
 * Nodes are source, relay-a, relay-b, sink, in that order.
 * Classes are URGENT, COMMAND, BACKGROUND, in that order.
 */

#include "net_state.h"

#include <stddef.h>
#include <stdint.h>

#define TELEM_TCP_PORT 8000
#define TELEM_UDP_PORT 8001
#define TELEM_REPORT_CAP 2048
#define TELEM_WINDOW 32

enum {
    TELEM_SOURCE = 0,
    TELEM_RELAY_A = 1,
    TELEM_RELAY_B = 2,
    TELEM_SINK = 3,
    TELEM_NODES = 4
};

enum telem_mode {
    TELEM_MODE_UNKNOWN = 0,
    TELEM_MODE_NORMAL = 1,
    TELEM_MODE_FLOOD = 2,
    TELEM_MODE_MUTE = 3
};

struct telem_class {
    uint64_t issued;
    uint32_t on_time;
    uint32_t late;
    uint32_t lost;
    uint32_t dropped;
    int have_latency;
    int64_t median_ns;
    int64_t max_ns;
};

/*
 * Whether the books outlived restarts (D-063). source reports its own
 * part and the numbers that wait to be closed, sink its own part. A
 * reset is 1 when a restarted node found no books to go on from.
 */
struct telem_books {
    uint32_t source_resumes;
    uint32_t source_reset;
    uint64_t waiting;
    uint32_t sink_resumes;
    uint32_t sink_reset;
};

/* alive is -1 until a nodes report, 0 dead, 1 alive. */
struct telem_node {
    int alive;
    uint32_t restarts;
};

struct telem_snapshot {
    uint64_t seq;
    int64_t time_ns;
    enum net_state state;
    enum telem_mode source_mode;
    struct telem_node node[TELEM_NODES];
    struct telem_class klass[3];
    struct telem_books books;
};

struct telem_window {
    int64_t value[TELEM_WINDOW];
    uint32_t count;
    uint32_t next;
};

void telem_window_init(struct telem_window *window);
void telem_window_add(struct telem_window *window, int64_t value);

/* 0 and writes *out. -1 if window or out is NULL, or the window is empty. */
int telem_window_median(const struct telem_window *window, int64_t *out);
int telem_window_max(const struct telem_window *window, int64_t *out);

/* Copies the window into the class latency fields. Empty clears them. */
void telem_class_set_latency(struct telem_class *klass,
                             const struct telem_window *window);

void telem_snapshot_init(struct telem_snapshot *snap);

/* Flood wins when both flags are set. Urgent on and flood off is normal. */
enum telem_mode telem_mode_from_flags(int flood, int urgent_on);
const char *telem_mode_name(enum telem_mode mode);

/* Bytes written, excluding the NUL. -1 if the buffer is too small or
 * an argument is NULL. */
int telem_report_source(char *buf, size_t cap, enum telem_mode mode,
                        uint64_t issued_urgent, uint64_t issued_command,
                        uint64_t issued_background,
                        const struct telem_books *books);
int telem_report_sink(char *buf, size_t cap, enum net_state state,
                      const struct telem_class klass[3],
                      const struct telem_books *books);
int telem_report_nodes(char *buf, size_t cap,
                       const struct telem_node node[TELEM_NODES]);

/*
 * One kind-report. Fields that report does not carry stay as they were.
 * 0 on a complete report. -1 leaves snap unchanged.
 */
int telem_snapshot_merge_report(struct telem_snapshot *snap, const char *text);

/* One TCP snapshot, ending in "end\n". */
int telem_snapshot_format(const struct telem_snapshot *snap, char *buf,
                          size_t cap);

/* 0 on a complete snapshot. -1 leaves out unchanged. */
int telem_snapshot_parse(const char *text, struct telem_snapshot *out);

#endif
