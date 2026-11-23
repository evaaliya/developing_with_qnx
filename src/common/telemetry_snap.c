#include "telemetry_snap.h"

#include "latency_stats.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void telem_window_init(struct telem_window *window)
{
    if (window == NULL) {
        return;
    }
    memset(window, 0, sizeof *window);
}

void telem_window_add(struct telem_window *window, int64_t value)
{
    if (window == NULL) {
        return;
    }
    window->value[window->next] = value;
    window->next++;
    if (window->next >= TELEM_WINDOW) {
        window->next = 0;
    }
    if (window->count < TELEM_WINDOW) {
        window->count++;
    }
}

static int window_to_sample(const struct telem_window *window,
                            struct latency_sample *sample)
{
    uint32_t i;

    if (window == NULL || window->count == 0 ||
        window->count > TELEM_WINDOW) {
        return -1;
    }
    latency_sample_init(sample);
    for (i = 0; i < window->count; i++) {
        if (latency_sample_add(sample, window->value[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

int telem_window_median(const struct telem_window *window, int64_t *out)
{
    struct latency_sample sample;

    if (out == NULL || window_to_sample(window, &sample) != 0) {
        return -1;
    }
    return latency_stats_median(&sample, out);
}

int telem_window_max(const struct telem_window *window, int64_t *out)
{
    struct latency_sample sample;

    if (out == NULL || window_to_sample(window, &sample) != 0) {
        return -1;
    }
    return latency_stats_max(&sample, out);
}

void telem_class_set_latency(struct telem_class *klass,
                             const struct telem_window *window)
{
    int64_t median;
    int64_t maximum;

    if (klass == NULL) {
        return;
    }
    if (window == NULL || telem_window_median(window, &median) != 0 ||
        telem_window_max(window, &maximum) != 0) {
        klass->have_latency = 0;
        klass->median_ns = 0;
        klass->max_ns = 0;
        return;
    }
    klass->have_latency = 1;
    klass->median_ns = median;
    klass->max_ns = maximum;
}

void telem_snapshot_init(struct telem_snapshot *snap)
{
    int i;

    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof *snap);
    snap->state = NET_INIT;
    snap->source_mode = TELEM_MODE_UNKNOWN;
    for (i = 0; i < TELEM_NODES; i++) {
        snap->node[i].alive = -1;
    }
}

enum telem_mode telem_mode_from_flags(int flood, int urgent_on)
{
    if (flood) {
        return TELEM_MODE_FLOOD;
    }
    if (!urgent_on) {
        return TELEM_MODE_MUTE;
    }
    return TELEM_MODE_NORMAL;
}

const char *telem_mode_name(enum telem_mode mode)
{
    switch (mode) {
    case TELEM_MODE_NORMAL:
        return "normal";
    case TELEM_MODE_FLOOD:
        return "flood";
    case TELEM_MODE_MUTE:
        return "mute";
    case TELEM_MODE_UNKNOWN:
    default:
        return "unknown";
    }
}

static int append_text(char *buf, size_t cap, size_t *used, const char *fmt,
                       ...)
{
    va_list ap;
    int wrote;

    if (*used >= cap) {
        return -1;
    }
    va_start(ap, fmt);
    wrote = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (wrote < 0 || (size_t)wrote >= cap - *used) {
        return -1;
    }
    *used += (size_t)wrote;
    return 0;
}

static const char *node_name(int index)
{
    switch (index) {
    case TELEM_SOURCE:
        return "source";
    case TELEM_RELAY_A:
        return "relay-a";
    case TELEM_RELAY_B:
        return "relay-b";
    case TELEM_SINK:
        return "sink";
    default:
        return NULL;
    }
}

static int node_index(const char *name)
{
    int i;

    for (i = 0; i < TELEM_NODES; i++) {
        if (strcmp(name, node_name(i)) == 0) {
            return i;
        }
    }
    return -1;
}

static int class_index(const char *name)
{
    if (strcmp(name, "URGENT") == 0) {
        return 0;
    }
    if (strcmp(name, "COMMAND") == 0) {
        return 1;
    }
    if (strcmp(name, "BACKGROUND") == 0) {
        return 2;
    }
    return -1;
}

static int append_node(char *buf, size_t cap, size_t *used,
                       const struct telem_node *node, int index)
{
    const char *name = node_name(index);

    if (name == NULL || node->alive < -1 || node->alive > 1) {
        return -1;
    }
    if (node->alive < 0) {
        return append_text(buf, cap, used,
                           "node %s alive unknown restarts %u\n", name,
                           node->restarts);
    }
    return append_text(buf, cap, used, "node %s alive %d restarts %u\n", name,
                       node->alive, node->restarts);
}

static int append_latency_token(char *buf, size_t cap, size_t *used,
                                const char *key, int have, int64_t value)
{
    if (!have) {
        return append_text(buf, cap, used, " %s none", key);
    }
    return append_text(buf, cap, used, " %s %" PRId64, key, value);
}

static int append_class_full(char *buf, size_t cap, size_t *used,
                             const struct telem_class *klass, uint32_t id)
{
    if (append_text(buf, cap, used,
                    "class %s issued %" PRIu64
                    " on_time %u late %u lost %u dropped %u",
                    signal_class_name(id), klass->issued, klass->on_time,
                    klass->late, klass->lost, klass->dropped) != 0) {
        return -1;
    }
    if (append_latency_token(buf, cap, used, "median_ns", klass->have_latency,
                             klass->median_ns) != 0 ||
        append_latency_token(buf, cap, used, "max_ns", klass->have_latency,
                             klass->max_ns) != 0) {
        return -1;
    }
    return append_text(buf, cap, used, "\n");
}

int telem_report_source(char *buf, size_t cap, enum telem_mode mode,
                        uint64_t issued_urgent, uint64_t issued_command,
                        uint64_t issued_background)
{
    size_t used = 0;
    uint64_t values[3];
    int i;

    if (buf == NULL || cap == 0) {
        return -1;
    }
    values[0] = issued_urgent;
    values[1] = issued_command;
    values[2] = issued_background;
    if (append_text(buf, cap, &used, "kind source\n") != 0 ||
        append_text(buf, cap, &used, "source_mode %s\n",
                    telem_mode_name(mode)) != 0) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        if (append_text(buf, cap, &used, "class %s issued %" PRIu64 "\n",
                        signal_class_name((uint32_t)(i + 1)),
                        values[i]) != 0) {
            return -1;
        }
    }
    if (append_text(buf, cap, &used, "end\n") != 0) {
        return -1;
    }
    return (int)used;
}

int telem_report_sink(char *buf, size_t cap, enum net_state state,
                      const struct telem_class klass[3])
{
    size_t used = 0;
    int i;

    if (buf == NULL || cap == 0 || klass == NULL ||
        net_state_name(state) == NULL) {
        return -1;
    }
    if (append_text(buf, cap, &used, "kind sink\n") != 0 ||
        append_text(buf, cap, &used, "state %s\n", net_state_name(state)) !=
            0) {
        return -1;
    }
    for (i = 0; i < 3; i++) {
        if (append_text(buf, cap, &used,
                        "class %s on_time %u late %u lost %u dropped %u",
                        signal_class_name((uint32_t)(i + 1)),
                        klass[i].on_time, klass[i].late, klass[i].lost,
                        klass[i].dropped) != 0 ||
            append_latency_token(buf, cap, &used, "median_ns",
                                 klass[i].have_latency,
                                 klass[i].median_ns) != 0 ||
            append_latency_token(buf, cap, &used, "max_ns",
                                 klass[i].have_latency, klass[i].max_ns) !=
                0 ||
            append_text(buf, cap, &used, "\n") != 0) {
            return -1;
        }
    }
    if (append_text(buf, cap, &used, "end\n") != 0) {
        return -1;
    }
    return (int)used;
}

int telem_report_nodes(char *buf, size_t cap,
                       const struct telem_node node[TELEM_NODES])
{
    size_t used = 0;
    int i;

    if (buf == NULL || cap == 0 || node == NULL) {
        return -1;
    }
    if (append_text(buf, cap, &used, "kind nodes\n") != 0) {
        return -1;
    }
    for (i = 0; i < TELEM_NODES; i++) {
        if (append_node(buf, cap, &used, &node[i], i) != 0) {
            return -1;
        }
    }
    if (append_text(buf, cap, &used, "end\n") != 0) {
        return -1;
    }
    return (int)used;
}

int telem_snapshot_format(const struct telem_snapshot *snap, char *buf,
                          size_t cap)
{
    size_t used = 0;
    int i;

    if (snap == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    if (append_text(buf, cap, &used, "seq %" PRIu64 "\n", snap->seq) != 0 ||
        append_text(buf, cap, &used, "time_ns %" PRId64 "\n", snap->time_ns) !=
            0 ||
        append_text(buf, cap, &used, "state %s\n",
                    net_state_name(snap->state)) != 0 ||
        append_text(buf, cap, &used, "source_mode %s\n",
                    telem_mode_name(snap->source_mode)) != 0) {
        return -1;
    }
    for (i = 0; i < TELEM_NODES; i++) {
        if (append_node(buf, cap, &used, &snap->node[i], i) != 0) {
            return -1;
        }
    }
    for (i = 0; i < 3; i++) {
        if (append_class_full(buf, cap, &used, &snap->klass[i],
                              (uint32_t)(i + 1)) != 0) {
            return -1;
        }
    }
    if (append_text(buf, cap, &used, "end\n") != 0) {
        return -1;
    }
    return (int)used;
}

static int split_words(char *line, char **words, int max)
{
    int n = 0;

    while (*line != '\0' && n < max) {
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0') {
            break;
        }
        words[n++] = line;
        while (*line != '\0' && *line != ' ' && *line != '\t') {
            line++;
        }
        if (*line != '\0') {
            *line++ = '\0';
        }
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    if (*line != '\0') {
        return -1;
    }
    return n;
}

static int parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0 || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int parse_u64(const char *text, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

static int parse_i64(const char *text, int64_t *out)
{
    char *end = NULL;
    long long value;

    if (text == NULL || text[0] == '\0') {
        return -1;
    }
    errno = 0;
    value = strtoll(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = (int64_t)value;
    return 0;
}

static int parse_mode(const char *text, enum telem_mode *out)
{
    if (strcmp(text, "unknown") == 0) {
        *out = TELEM_MODE_UNKNOWN;
    } else if (strcmp(text, "normal") == 0) {
        *out = TELEM_MODE_NORMAL;
    } else if (strcmp(text, "flood") == 0) {
        *out = TELEM_MODE_FLOOD;
    } else if (strcmp(text, "mute") == 0) {
        *out = TELEM_MODE_MUTE;
    } else {
        return -1;
    }
    return 0;
}

static int parse_state(const char *text, enum net_state *out)
{
    static const enum net_state states[] = {
        NET_INIT, NET_NORMAL, NET_DEGRADED, NET_SAFE, NET_RECOVERING,
        NET_LOCKED_SAFE
    };
    size_t i;

    for (i = 0; i < sizeof states / sizeof states[0]; i++) {
        if (strcmp(text, net_state_name(states[i])) == 0) {
            *out = states[i];
            return 0;
        }
    }
    return -1;
}

static int parse_latency_token(const char *text, int *have, int64_t *out)
{
    if (strcmp(text, "none") == 0) {
        *have = 0;
        *out = 0;
        return 0;
    }
    if (parse_i64(text, out) != 0) {
        return -1;
    }
    *have = 1;
    return 0;
}

static int parse_alive(const char *text, int *alive)
{
    if (strcmp(text, "unknown") == 0) {
        *alive = -1;
        return 0;
    }
    if (strcmp(text, "0") == 0) {
        *alive = 0;
        return 0;
    }
    if (strcmp(text, "1") == 0) {
        *alive = 1;
        return 0;
    }
    return -1;
}

enum report_kind {
    KIND_NONE = 0,
    KIND_FULL = 1,
    KIND_SINK = 2,
    KIND_SOURCE = 3,
    KIND_NODES = 4
};

struct parse_flags {
    enum report_kind kind;
    int seq;
    int time_ns;
    int state;
    int mode;
    int node[TELEM_NODES];
    int class_full[3];
    int class_issued[3];
    int class_sink[3];
    int ended;
};

static int take_class_issued(struct telem_snapshot *snap, char **words, int n,
                             struct parse_flags *flags)
{
    int index;
    uint64_t issued;

    if (n != 4 || strcmp(words[2], "issued") != 0) {
        return -1;
    }
    index = class_index(words[1]);
    if (index < 0 || flags->class_issued[index] ||
        parse_u64(words[3], &issued) != 0) {
        return -1;
    }
    snap->klass[index].issued = issued;
    flags->class_issued[index] = 1;
    return 0;
}

static int take_class_sink(struct telem_snapshot *snap, char **words, int n,
                           struct parse_flags *flags)
{
    int index;
    int have_median = 0;
    int have_max = 0;
    uint32_t on_time;
    uint32_t late;
    uint32_t lost;
    uint32_t dropped;
    int64_t median = 0;
    int64_t maximum = 0;

    if (n != 14 || strcmp(words[2], "on_time") != 0 ||
        strcmp(words[4], "late") != 0 || strcmp(words[6], "lost") != 0 ||
        strcmp(words[8], "dropped") != 0 ||
        strcmp(words[10], "median_ns") != 0 ||
        strcmp(words[12], "max_ns") != 0) {
        return -1;
    }
    index = class_index(words[1]);
    if (index < 0 || flags->class_sink[index] ||
        parse_u32(words[3], &on_time) != 0 ||
        parse_u32(words[5], &late) != 0 || parse_u32(words[7], &lost) != 0 ||
        parse_u32(words[9], &dropped) != 0 ||
        parse_latency_token(words[11], &have_median, &median) != 0 ||
        parse_latency_token(words[13], &have_max, &maximum) != 0 ||
        have_median != have_max) {
        return -1;
    }
    snap->klass[index].on_time = on_time;
    snap->klass[index].late = late;
    snap->klass[index].lost = lost;
    snap->klass[index].dropped = dropped;
    snap->klass[index].have_latency = have_median;
    snap->klass[index].median_ns = median;
    snap->klass[index].max_ns = maximum;
    flags->class_sink[index] = 1;
    return 0;
}

static int take_class_full(struct telem_snapshot *snap, char **words, int n,
                           struct parse_flags *flags)
{
    int index;
    int have_median = 0;
    int have_max = 0;
    uint64_t issued;
    uint32_t on_time;
    uint32_t late;
    uint32_t lost;
    uint32_t dropped;
    int64_t median = 0;
    int64_t maximum = 0;

    if (n != 16 || strcmp(words[2], "issued") != 0 ||
        strcmp(words[4], "on_time") != 0 || strcmp(words[6], "late") != 0 ||
        strcmp(words[8], "lost") != 0 || strcmp(words[10], "dropped") != 0 ||
        strcmp(words[12], "median_ns") != 0 ||
        strcmp(words[14], "max_ns") != 0) {
        return -1;
    }
    index = class_index(words[1]);
    if (index < 0 || flags->class_full[index] ||
        parse_u64(words[3], &issued) != 0 ||
        parse_u32(words[5], &on_time) != 0 ||
        parse_u32(words[7], &late) != 0 || parse_u32(words[9], &lost) != 0 ||
        parse_u32(words[11], &dropped) != 0 ||
        parse_latency_token(words[13], &have_median, &median) != 0 ||
        parse_latency_token(words[15], &have_max, &maximum) != 0 ||
        have_median != have_max) {
        return -1;
    }
    snap->klass[index].issued = issued;
    snap->klass[index].on_time = on_time;
    snap->klass[index].late = late;
    snap->klass[index].lost = lost;
    snap->klass[index].dropped = dropped;
    snap->klass[index].have_latency = have_median;
    snap->klass[index].median_ns = median;
    snap->klass[index].max_ns = maximum;
    flags->class_full[index] = 1;
    return 0;
}

static int take_node(struct telem_snapshot *snap, char **words, int n,
                     struct parse_flags *flags)
{
    int index;
    int alive;
    uint32_t restarts;

    if (n != 6 || strcmp(words[2], "alive") != 0 ||
        strcmp(words[4], "restarts") != 0) {
        return -1;
    }
    index = node_index(words[1]);
    if (index < 0 || flags->node[index] || parse_alive(words[3], &alive) != 0 ||
        parse_u32(words[5], &restarts) != 0) {
        return -1;
    }
    snap->node[index].alive = alive;
    snap->node[index].restarts = restarts;
    flags->node[index] = 1;
    return 0;
}

static int flags_complete(const struct parse_flags *flags)
{
    int i;

    if (!flags->ended) {
        return 0;
    }
    if (flags->kind == KIND_FULL) {
        if (!flags->seq || !flags->time_ns || !flags->state || !flags->mode) {
            return 0;
        }
        for (i = 0; i < TELEM_NODES; i++) {
            if (!flags->node[i]) {
                return 0;
            }
        }
        for (i = 0; i < 3; i++) {
            if (!flags->class_full[i]) {
                return 0;
            }
        }
        return 1;
    }
    if (flags->kind == KIND_SINK) {
        if (!flags->state) {
            return 0;
        }
        for (i = 0; i < 3; i++) {
            if (!flags->class_sink[i]) {
                return 0;
            }
        }
        return 1;
    }
    if (flags->kind == KIND_SOURCE) {
        if (!flags->mode) {
            return 0;
        }
        for (i = 0; i < 3; i++) {
            if (!flags->class_issued[i]) {
                return 0;
            }
        }
        return 1;
    }
    if (flags->kind == KIND_NODES) {
        for (i = 0; i < TELEM_NODES; i++) {
            if (!flags->node[i]) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

static int take_line(struct telem_snapshot *snap, char *line,
                     struct parse_flags *flags)
{
    char *words[20];
    int n;

    n = split_words(line, words, 20);
    if (n <= 0) {
        return 0;
    }
    if (flags->ended) {
        return -1;
    }
    if (strcmp(words[0], "end") == 0) {
        if (n != 1) {
            return -1;
        }
        flags->ended = 1;
        return 0;
    }
    if (strcmp(words[0], "kind") == 0) {
        if (n != 2 || flags->kind != KIND_NONE) {
            return -1;
        }
        if (strcmp(words[1], "sink") == 0) {
            flags->kind = KIND_SINK;
        } else if (strcmp(words[1], "source") == 0) {
            flags->kind = KIND_SOURCE;
        } else if (strcmp(words[1], "nodes") == 0) {
            flags->kind = KIND_NODES;
        } else {
            return -1;
        }
        return 0;
    }
    if (strcmp(words[0], "seq") == 0) {
        uint64_t seq;

        if (flags->kind != KIND_NONE && flags->kind != KIND_FULL) {
            return -1;
        }
        flags->kind = KIND_FULL;
        if (n != 2 || flags->seq || parse_u64(words[1], &seq) != 0) {
            return -1;
        }
        snap->seq = seq;
        flags->seq = 1;
        return 0;
    }
    if (strcmp(words[0], "time_ns") == 0) {
        int64_t time_ns;

        if (flags->kind != KIND_NONE && flags->kind != KIND_FULL) {
            return -1;
        }
        flags->kind = KIND_FULL;
        if (n != 2 || flags->time_ns || parse_i64(words[1], &time_ns) != 0) {
            return -1;
        }
        snap->time_ns = time_ns;
        flags->time_ns = 1;
        return 0;
    }
    if (strcmp(words[0], "state") == 0) {
        enum net_state state;

        if (n != 2 || flags->state || parse_state(words[1], &state) != 0) {
            return -1;
        }
        if (flags->kind != KIND_NONE && flags->kind != KIND_FULL &&
            flags->kind != KIND_SINK) {
            return -1;
        }
        if (flags->kind == KIND_NONE) {
            flags->kind = KIND_FULL;
        }
        snap->state = state;
        flags->state = 1;
        return 0;
    }
    if (strcmp(words[0], "source_mode") == 0) {
        enum telem_mode mode;

        if (n != 2 || flags->mode || parse_mode(words[1], &mode) != 0) {
            return -1;
        }
        if (flags->kind != KIND_NONE && flags->kind != KIND_FULL &&
            flags->kind != KIND_SOURCE) {
            return -1;
        }
        if (flags->kind == KIND_NONE) {
            flags->kind = KIND_FULL;
        }
        snap->source_mode = mode;
        flags->mode = 1;
        return 0;
    }
    if (strcmp(words[0], "node") == 0) {
        if (flags->kind != KIND_NONE && flags->kind != KIND_FULL &&
            flags->kind != KIND_NODES) {
            return -1;
        }
        if (flags->kind == KIND_NONE) {
            flags->kind = KIND_FULL;
        }
        return take_node(snap, words, n, flags);
    }
    if (strcmp(words[0], "class") == 0) {
        if (flags->kind == KIND_SOURCE ||
            (flags->kind == KIND_NONE && n == 4)) {
            if (flags->kind == KIND_NONE) {
                return -1;
            }
            return take_class_issued(snap, words, n, flags);
        }
        if (flags->kind == KIND_SINK) {
            return take_class_sink(snap, words, n, flags);
        }
        if (flags->kind == KIND_NONE) {
            flags->kind = KIND_FULL;
        }
        if (flags->kind != KIND_FULL) {
            return -1;
        }
        return take_class_full(snap, words, n, flags);
    }
    return -1;
}

static int parse_text(struct telem_snapshot *snap, const char *text,
                      enum report_kind expect)
{
    struct parse_flags flags;
    char copy[TELEM_REPORT_CAP];
    size_t length;
    char *cursor;
    char *line;

    if (snap == NULL || text == NULL) {
        return -1;
    }
    length = strlen(text);
    if (length == 0 || length >= sizeof copy) {
        return -1;
    }
    memcpy(copy, text, length + 1);
    memset(&flags, 0, sizeof flags);
    cursor = copy;
    while (cursor != NULL) {
        line = cursor;
        cursor = strchr(cursor, '\n');
        if (cursor != NULL) {
            *cursor++ = '\0';
        }
        if (line[0] != '\0' && line[strlen(line) - 1] == '\r') {
            line[strlen(line) - 1] = '\0';
        }
        if (take_line(snap, line, &flags) != 0) {
            return -1;
        }
    }
    if (!flags_complete(&flags) || flags.kind != expect) {
        return -1;
    }
    return 0;
}

int telem_snapshot_merge_report(struct telem_snapshot *snap, const char *text)
{
    struct telem_snapshot next;
    struct parse_flags flags;
    char copy[TELEM_REPORT_CAP];
    size_t length;
    char *cursor;
    char *line;

    if (snap == NULL || text == NULL) {
        return -1;
    }
    length = strlen(text);
    if (length == 0 || length >= sizeof copy) {
        return -1;
    }
    next = *snap;
    memcpy(copy, text, length + 1);
    memset(&flags, 0, sizeof flags);
    cursor = copy;
    while (cursor != NULL) {
        line = cursor;
        cursor = strchr(cursor, '\n');
        if (cursor != NULL) {
            *cursor++ = '\0';
        }
        if (line[0] != '\0' && line[strlen(line) - 1] == '\r') {
            line[strlen(line) - 1] = '\0';
        }
        if (take_line(&next, line, &flags) != 0) {
            return -1;
        }
    }
    if (!flags_complete(&flags) || flags.kind == KIND_FULL ||
        flags.kind == KIND_NONE) {
        return -1;
    }
    *snap = next;
    return 0;
}

int telem_snapshot_parse(const char *text, struct telem_snapshot *out)
{
    struct telem_snapshot next;

    if (out == NULL) {
        return -1;
    }
    telem_snapshot_init(&next);
    if (parse_text(&next, text, KIND_FULL) != 0) {
        return -1;
    }
    *out = next;
    return 0;
}
