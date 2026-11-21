/*
 * Host-side summary of sink latency lines. Compiled with the system cc,
 * not qcc. A line that starts with "sink: signal " and does not match the
 * sink format is counted. Other lines are not signal records, so they are
 * left out of that count. The percentile is omitted below 100 samples
 * (D-045).
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

static int add_value(struct latency_sample *sample, int64_t value,
                     const char *path)
{
    if (latency_sample_add(sample, value) == 0) {
        return 0;
    }
    fprintf(stderr, "latency-report: sample is full while reading %s\n", path);
    return -1;
}

static int read_log(const char *path, struct latency_sample *total,
                    struct latency_sample hops[][LATENCY_REPORT_HOPS],
                    unsigned *unparsed)
{
    FILE *file;
    char line[LATENCY_REPORT_LINE];

    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "latency-report: cannot read %s: %s\n", path,
                strerror(errno));
        return -1;
    }

    while (fgets(line, (int)sizeof line, file) != NULL) {
        struct parsed_signal parsed;
        size_t length = strlen(line);
        int status;
        int hop;

        if (length == sizeof line - 1 && line[length - 1] != '\n') {
            int ch;
            int signal_line = is_signal_line(line);

            while ((ch = fgetc(file)) != EOF && ch != '\n') {
            }
            if (signal_line) {
                (*unparsed)++;
            }
            continue;
        }

        status = parse_signal(line, &parsed);
        if (status == 0) {
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
    }

    if (ferror(file)) {
        fprintf(stderr, "latency-report: reading %s failed: %s\n", path,
                strerror(errno));
        fclose(file);
        return -1;
    }
    fclose(file);
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

int main(int argc, char **argv)
{
    static struct latency_sample total[LATENCY_REPORT_CLASSES];
    static struct latency_sample hops[LATENCY_REPORT_CLASSES][LATENCY_REPORT_HOPS];
    unsigned unparsed = 0;
    int arg;
    int class_id;
    int hop;

    if (argc < 2) {
        fprintf(stderr, "usage: latency-report LOG...\n");
        return EXIT_FAILURE;
    }

    for (class_id = 0; class_id < LATENCY_REPORT_CLASSES; class_id++) {
        latency_sample_init(&total[class_id]);
        for (hop = 0; hop < LATENCY_REPORT_HOPS; hop++) {
            latency_sample_init(&hops[class_id][hop]);
        }
    }

    for (arg = 1; arg < argc; arg++) {
        if (read_log(argv[arg], total, hops, &unparsed) != 0) {
            return EXIT_FAILURE;
        }
    }

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
    return EXIT_SUCCESS;
}
