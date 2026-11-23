/*
 * Host-side summary of sink latency lines. Compiled with the system cc,
 * not qcc. A line that starts with "sink: signal " and does not match the
 * sink format is counted. Other lines are not signal records, so they are
 * left out of that count. The percentile is omitted below 100 samples
 * (D-045).
 *
 * Relay send lines carry arrived, start, and end, and may end with
 * picked. The picked stamp is not part of R5. A less urgent send that
 * starts while a more urgent signal is already here and not yet started
 * is one R5 violation. A signal that arrives during another send waits
 * start - arrived. An older relay line, without those stamps, is counted
 * as unparsed and does not stop the report. A run is a stall when the
 * median of its relay-B-to-sink hop is above 20 ms (D-048, D-049).
 */

#include "latency_stats.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LATENCY_REPORT_CLASSES 3
/* Two relay stamps plus the sink's own leg, with room for one more stamp. */
#define LATENCY_REPORT_HOPS 5
#define LATENCY_REPORT_LINE 2048
#define ORDER_RELAYS 8
#define ORDER_SENDS 8192
#define ORDER_VIOLATIONS 256
#define ORDER_NAME 64
#define ORDER_PATH 512
#define STALL_RUNS 128
/* Third hop printed by sink: relay B to sink. */
#define STALL_HOP_INDEX 2
/* A stall median is strictly above 20 ms. */
#define STALL_MEDIAN_ABOVE_NS 20000000LL

static const char *class_name[LATENCY_REPORT_CLASSES] = {
    "URGENT",
    "COMMAND",
    "BACKGROUND"
};

struct parsed_signal {
    int class_index;
    int hop_count;
    int64_t hop_ns[LATENCY_REPORT_HOPS];
    int64_t total_ns;
};

static int class_index_of(const char *name)
{
    int i;

    for (i = 0; i < LATENCY_REPORT_CLASSES; i++) {
        if (strcmp(name, class_name[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static int is_signal_line(const char *line)
{
    return strncmp(line, "sink: signal ", 13) == 0;
}

/*
 * 1 when the line is a complete sink signal record.
 * 0 when it is not a signal line.
 * -1 when it claims to be a signal line and cannot be read.
 * The sink prints "on time" or "late", then an optional ", fresh" or
 * ", held", then ", hops", one or more "<n> ns" segments, and "total".
 */
static int parse_signal(const char *line, struct parsed_signal *out)
{
    const char *cursor;
    unsigned long long number;
    char name[32];
    int used = 0;
    int hop_count = 0;

    if (!is_signal_line(line)) {
        return 0;
    }
    cursor = line + 13;
    if (sscanf(cursor, "%llu class %31s %n", &number, name, &used) != 2 ||
        used <= 0 || number == 0) {
        return -1;
    }
    cursor += used;
    out->class_index = class_index_of(name);
    if (out->class_index < 0) {
        return -1;
    }
    if (strncmp(cursor, "on time", 7) == 0) {
        cursor += 7;
    } else if (strncmp(cursor, "late", 4) == 0) {
        cursor += 4;
    } else {
        return -1;
    }
    if (strncmp(cursor, ", fresh", 7) == 0) {
        cursor += 7;
    } else if (strncmp(cursor, ", held", 6) == 0) {
        cursor += 6;
    }
    if (strncmp(cursor, ", hops", 6) != 0) {
        return -1;
    }
    cursor += 6;

    for (;;) {
        int64_t value;
        int consumed = 0;

        if (hop_count >= LATENCY_REPORT_HOPS) {
            return -1;
        }
        if (sscanf(cursor, " %" SCNd64 " ns%n", &value, &consumed) != 1 ||
            consumed <= 0) {
            return -1;
        }
        cursor += consumed;
        if (strncmp(cursor, ", total ", 8) == 0) {
            int64_t total;

            cursor += 8;
            consumed = 0;
            if (sscanf(cursor, "%" SCNd64 " ns%n", &total, &consumed) != 1 ||
                consumed <= 0) {
                return -1;
            }
            cursor += consumed;
            while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
                   *cursor == '\n') {
                cursor++;
            }
            if (*cursor != '\0') {
                return -1;
            }
            out->hop_ns[hop_count] = value;
            hop_count++;
            out->hop_count = hop_count;
            out->total_ns = total;
            return 1;
        }
        if (*cursor != ',') {
            return -1;
        }
        cursor++;
        out->hop_ns[hop_count] = value;
        hop_count++;
    }
}

struct relay_send {
    uint64_t number;
    int class_index;
    int64_t arrived_ns;
    int64_t start_ns;
    int64_t end_ns;
};

struct tagged_send {
    int slot;
    struct relay_send send;
};

struct relay_acc {
    char name[ORDER_NAME];
    int used;
    struct latency_sample wait[LATENCY_REPORT_CLASSES];
    unsigned violations;
};

struct violation_line {
    int slot;
    char text[384];
};

struct stall_run {
    char path[ORDER_PATH];
    int64_t median_ns;
};

static struct relay_acc relays[ORDER_RELAYS];
static struct tagged_send file_sends[ORDER_SENDS];
static unsigned file_send_count;
static struct violation_line violations[ORDER_VIOLATIONS];
static unsigned violation_stored;
static struct stall_run stall_runs[STALL_RUNS];
static unsigned stall_count;
static unsigned relay_unparsed;
static struct latency_sample file_hop3;

static int add_value(struct latency_sample *sample, int64_t value,
                     const char *path)
{
    if (latency_sample_add(sample, value) == 0) {
        return 0;
    }
    fprintf(stderr, "latency-report: sample is full while reading %s\n", path);
    return -1;
}

static int relay_slot(const char *name)
{
    int i;
    int class_id;

    for (i = 0; i < ORDER_RELAYS; i++) {
        if (!relays[i].used) {
            if (strlen(name) == 0 || strlen(name) >= sizeof relays[i].name) {
                return -1;
            }
            relays[i].used = 1;
            snprintf(relays[i].name, sizeof relays[i].name, "%s", name);
            for (class_id = 0; class_id < LATENCY_REPORT_CLASSES; class_id++) {
                latency_sample_init(&relays[i].wait[class_id]);
            }
            return i;
        }
        if (strcmp(relays[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * 1 when the line is a relay send with arrived, start, and end.
 * 0 when it is not a "sent signal" line.
 * -1 when it is a send line that cannot be read, including the older
 * format that stops at "still waiting".
 */
static int parse_relay_send(const char *line, char *name, size_t name_len,
                            struct relay_send *out)
{
    const char *cursor;
    const char *mark;
    unsigned long long number;
    char class_token[32];
    int used = 0;
    int consumed = 0;
    size_t name_n;

    if (strncmp(line, "relay ", 6) != 0) {
        return 0;
    }
    cursor = line + 6;
    mark = strstr(cursor, ": sent signal ");
    if (mark == NULL) {
        return 0;
    }
    name_n = (size_t)(mark - cursor);
    if (name_n == 0 || name_n >= name_len) {
        return -1;
    }
    memcpy(name, cursor, name_n);
    name[name_n] = '\0';
    cursor = mark + 14; /* strlen(": sent signal ") */
    if (sscanf(cursor, "%llu class %31[^; \t]%n", &number, class_token,
               &used) != 2 ||
        used <= 0) {
        return -1;
    }
    out->number = (uint64_t)number;
    out->class_index = class_index_of(class_token);
    if (out->class_index < 0) {
        return -1;
    }
    cursor += used;
    mark = strstr(cursor, " arrived ");
    if (mark == NULL) {
        return -1;
    }
    if (sscanf(mark,
               " arrived %" SCNd64 " start %" SCNd64 " end %" SCNd64 "%n",
               &out->arrived_ns, &out->start_ns, &out->end_ns,
               &consumed) != 3 ||
        consumed <= 0) {
        return -1;
    }
    cursor = mark + consumed;
    if (strncmp(cursor, " picked ", 8) == 0) {
        int64_t picked_ns;
        int picked_used = 0;

        if (sscanf(cursor, " picked %" SCNd64 "%n", &picked_ns, &picked_used) != 1 ||
            picked_used <= 0) {
            return -1;
        }
        cursor += picked_used;
        (void)picked_ns;
    }
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' ||
           *cursor == '\n') {
        cursor++;
    }
    if (*cursor != '\0') {
        return -1;
    }
    return 1;
}

static int evaluate_file(const char *path)
{
    int slot;

    for (slot = 0; slot < ORDER_RELAYS; slot++) {
        unsigned i;
        unsigned j;
        unsigned count = 0;

        if (!relays[slot].used) {
            continue;
        }
        for (i = 0; i < file_send_count; i++) {
            if (file_sends[i].slot == slot) {
                count++;
            }
        }
        if (count == 0) {
            continue;
        }

        for (j = 0; j < file_send_count; j++) {
            int waited = 0;
            struct relay_send later;

            if (file_sends[j].slot != slot) {
                continue;
            }
            later = file_sends[j].send;
            for (i = 0; i < file_send_count; i++) {
                struct relay_send earlier;

                if (i == j || file_sends[i].slot != slot) {
                    continue;
                }
                earlier = file_sends[i].send;
                if (earlier.start_ns < later.arrived_ns &&
                    later.arrived_ns < earlier.end_ns) {
                    waited = 1;
                    break;
                }
            }
            if (!waited) {
                continue;
            }
            if (add_value(&relays[slot].wait[later.class_index],
                          later.start_ns - later.arrived_ns, path) != 0) {
                return -1;
            }
        }

        for (i = 0; i < file_send_count; i++) {
            struct relay_send less;

            if (file_sends[i].slot != slot) {
                continue;
            }
            less = file_sends[i].send;
            for (j = 0; j < file_send_count; j++) {
                struct relay_send more;
                int wrote;

                if (i == j || file_sends[j].slot != slot) {
                    continue;
                }
                more = file_sends[j].send;
                if (less.class_index <= more.class_index) {
                    continue;
                }
                if (!(more.arrived_ns < less.start_ns &&
                      less.start_ns < more.start_ns)) {
                    continue;
                }
                relays[slot].violations++;
                if (violation_stored >= ORDER_VIOLATIONS) {
                    continue;
                }
                wrote = snprintf(
                    violations[violation_stored].text,
                    sizeof violations[violation_stored].text,
                    "relay %s R5 violation %s signal %" PRIu64
                    " class %s start %" PRId64 "; signal %" PRIu64
                    " class %s arrived %" PRId64 " start %" PRId64 "\n",
                    relays[slot].name, path, less.number,
                    class_name[less.class_index], less.start_ns, more.number,
                    class_name[more.class_index], more.arrived_ns,
                    more.start_ns);
                if (wrote < 0 ||
                    (size_t)wrote >= sizeof violations[violation_stored].text) {
                    fprintf(stderr,
                            "latency-report: R5 line does not fit for %s\n",
                            path);
                    return -1;
                }
                violations[violation_stored].slot = slot;
                violation_stored++;
            }
        }
    }
    return 0;
}

static int note_stall(const char *path)
{
    int64_t median_ns = 0;
    int wrote;

    if (file_hop3.count == 0) {
        return 0;
    }
    if (latency_stats_median(&file_hop3, &median_ns) != 0) {
        fprintf(stderr, "latency-report: stall median failed for %s\n", path);
        return -1;
    }
    if (median_ns <= STALL_MEDIAN_ABOVE_NS) {
        return 0;
    }
    if (stall_count >= STALL_RUNS) {
        fprintf(stderr, "latency-report: too many stall runs\n");
        return -1;
    }
    wrote = snprintf(stall_runs[stall_count].path,
                     sizeof stall_runs[stall_count].path, "%s", path);
    if (wrote < 0 || (size_t)wrote >= sizeof stall_runs[stall_count].path) {
        fprintf(stderr, "latency-report: stall path does not fit: %s\n", path);
        return -1;
    }
    stall_runs[stall_count].median_ns = median_ns;
    stall_count++;
    return 0;
}

static int read_log(const char *path, struct latency_sample *total,
                    struct latency_sample hops[][LATENCY_REPORT_HOPS],
                    unsigned *unparsed)
{
    FILE *file;
    char line[LATENCY_REPORT_LINE];

    latency_sample_init(&file_hop3);
    file_send_count = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "latency-report: cannot read %s: %s\n", path,
                strerror(errno));
        return -1;
    }

    while (fgets(line, (int)sizeof line, file) != NULL) {
        struct parsed_signal parsed;
        struct relay_send send;
        char relay_name[ORDER_NAME];
        size_t length = strlen(line);
        int status;
        int relay_status;
        int slot;
        int hop;

        if (length == sizeof line - 1 && line[length - 1] != '\n') {
            int ch;
            int signal_line = is_signal_line(line);
            int relay_line = strncmp(line, "relay ", 6) == 0;

            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
            if (signal_line) {
                (*unparsed)++;
            } else if (relay_line) {
                relay_unparsed++;
            }
            continue;
        }

        status = parse_signal(line, &parsed);
        if (status == 0) {
            relay_status = parse_relay_send(line, relay_name, sizeof relay_name,
                                            &send);
            if (relay_status == 0) {
                continue;
            }
            if (relay_status < 0) {
                relay_unparsed++;
                continue;
            }
            slot = relay_slot(relay_name);
            if (slot < 0) {
                fprintf(stderr, "latency-report: too many relays in %s\n",
                        path);
                fclose(file);
                return -1;
            }
            if (file_send_count >= ORDER_SENDS) {
                fprintf(stderr, "latency-report: too many relay sends in %s\n",
                        path);
                fclose(file);
                return -1;
            }
            file_sends[file_send_count].slot = slot;
            file_sends[file_send_count].send = send;
            file_send_count++;
            continue;
        }
        if (status < 0) {
            (*unparsed)++;
            continue;
        }
        if (add_value(&total[parsed.class_index], parsed.total_ns, path) != 0) {
            fclose(file);
            return -1;
        }
        for (hop = 0; hop < parsed.hop_count; hop++) {
            if (add_value(&hops[parsed.class_index][hop], parsed.hop_ns[hop],
                          path) != 0) {
                fclose(file);
                return -1;
            }
        }
        if (parsed.hop_count > STALL_HOP_INDEX &&
            add_value(&file_hop3, parsed.hop_ns[STALL_HOP_INDEX], path) != 0) {
            fclose(file);
            return -1;
        }
    }

    if (ferror(file)) {
        fprintf(stderr, "latency-report: reading %s failed: %s\n", path,
                strerror(errno));
        fclose(file);
        return -1;
    }
    fclose(file);
    if (evaluate_file(path) != 0 || note_stall(path) != 0) {
        return -1;
    }
    return 0;
}

static void print_series(const char *class_label, const char *which,
                         const struct latency_sample *sample)
{
    int64_t median_ns = 0;
    int64_t max_ns = 0;
    int64_t p99_ns = 0;
    int p99_rc;

    if (sample->count == 0) {
        return;
    }
    if (latency_stats_median(sample, &median_ns) != 0 ||
        latency_stats_max(sample, &max_ns) != 0) {
        fprintf(stderr, "latency-report: statistics failed for %s %s\n",
                class_label, which);
        exit(EXIT_FAILURE);
    }
    p99_rc = latency_stats_p99(sample, &p99_ns);
    if (p99_rc < 0) {
        fprintf(stderr, "latency-report: percentile failed for %s %s\n",
                class_label, which);
        exit(EXIT_FAILURE);
    }
    printf("latency class %s %s n %" PRIu32 " median %" PRId64 " ns ",
           class_label, which, sample->count, median_ns);
    if (p99_rc == 1) {
        printf("p99 n/a ");
    } else {
        printf("p99 %" PRId64 " ns ", p99_ns);
    }
    printf("max %" PRId64 " ns\n", max_ns);
}

static void print_wait(const char *relay, const char *class_label,
                       const struct latency_sample *sample)
{
    int64_t median_ns = 0;
    int64_t max_ns = 0;
    int64_t p99_ns = 0;
    int p99_rc;

    if (sample->count == 0) {
        printf("relay %s wait behind send class %s n 0\n", relay, class_label);
        return;
    }
    if (latency_stats_median(sample, &median_ns) != 0 ||
        latency_stats_max(sample, &max_ns) != 0) {
        fprintf(stderr, "latency-report: wait statistics failed for %s %s\n",
                relay, class_label);
        exit(EXIT_FAILURE);
    }
    p99_rc = latency_stats_p99(sample, &p99_ns);
    if (p99_rc < 0) {
        fprintf(stderr, "latency-report: wait percentile failed for %s %s\n",
                relay, class_label);
        exit(EXIT_FAILURE);
    }
    printf("relay %s wait behind send class %s n %" PRIu32 " median %" PRId64
           " ns ",
           relay, class_label, sample->count, median_ns);
    if (p99_rc == 1) {
        printf("p99 n/a ");
    } else {
        printf("p99 %" PRId64 " ns ", p99_ns);
    }
    printf("max %" PRId64 " ns\n", max_ns);
}

static void print_order(void)
{
    int slot;
    int any = 0;

    for (slot = 0; slot < ORDER_RELAYS; slot++) {
        unsigned shown = 0;
        unsigned v;
        int class_id;

        if (!relays[slot].used) {
            continue;
        }
        any = 1;
        printf("relay %s R5 violations %u\n", relays[slot].name,
               relays[slot].violations);
        for (v = 0; v < violation_stored; v++) {
            if (violations[v].slot != slot) {
                continue;
            }
            fputs(violations[v].text, stdout);
            shown++;
        }
        if (shown < relays[slot].violations) {
            printf("relay %s R5 violations not all shown\n", relays[slot].name);
        }
        for (class_id = 0; class_id < LATENCY_REPORT_CLASSES; class_id++) {
            print_wait(relays[slot].name, class_name[class_id],
                       &relays[slot].wait[class_id]);
        }
    }
    if (!any) {
        printf("relay lines 0\n");
    }
    printf("relay unparsed %u\n", relay_unparsed);
}

static void print_stalls(void)
{
    unsigned i;

    printf("stall runs %u\n", stall_count);
    for (i = 0; i < stall_count; i++) {
        printf("stall run %s median %" PRId64 " ns\n", stall_runs[i].path,
               stall_runs[i].median_ns);
    }
}

int main(int argc, char **argv)
{
    static struct latency_sample total[LATENCY_REPORT_CLASSES];
    static struct latency_sample hops[LATENCY_REPORT_CLASSES][LATENCY_REPORT_HOPS];
    unsigned unparsed = 0;
    int stalls_only = 0;
    int first = 1;
    int arg;
    int class_id;
    int hop;

    if (argc >= 2 && strcmp(argv[1], "--stalls") == 0) {
        stalls_only = 1;
        first = 2;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: latency-report [--stalls] LOG...\n");
        return EXIT_FAILURE;
    }

    for (class_id = 0; class_id < LATENCY_REPORT_CLASSES; class_id++) {
        latency_sample_init(&total[class_id]);
        for (hop = 0; hop < LATENCY_REPORT_HOPS; hop++) {
            latency_sample_init(&hops[class_id][hop]);
        }
    }

    for (arg = first; arg < argc; arg++) {
        if (read_log(argv[arg], total, hops, &unparsed) != 0) {
            return EXIT_FAILURE;
        }
    }

    if (!stalls_only) {
        for (class_id = 0; class_id < LATENCY_REPORT_CLASSES; class_id++) {
            printf("latency class %s n %" PRIu32 "\n", class_name[class_id],
                   total[class_id].count);
            print_series(class_name[class_id], "total", &total[class_id]);
            for (hop = 0; hop < LATENCY_REPORT_HOPS; hop++) {
                char which[16];

                if (hops[class_id][hop].count == 0) {
                    continue;
                }
                snprintf(which, sizeof which, "hop %d", hop + 1);
                print_series(class_name[class_id], which, &hops[class_id][hop]);
            }
        }
        printf("latency unparsed %u\n", unparsed);
        print_order();
    }
    print_stalls();
    return EXIT_SUCCESS;
}
