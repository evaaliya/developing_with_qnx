#include "qt_core.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fit(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (buf == NULL || cap == 0) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
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

int qt_parse_opts(int argc, char *const argv[], struct qt_opts *opts)
{
    int i = 1;

    if (opts == NULL || argv == NULL) {
        return -1;
    }
    opts->out = NULL;
    opts->quiet = 0;
    while (i < argc) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                return -1;
            }
            opts->out = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-q") == 0) {
            opts->quiet = 1;
            i++;
        } else {
            break;
        }
    }
    return i;
}

int qt_parse_source(int argc, char *const argv[], int first,
                    const char **next_name)
{
    if (first < 1 || argc - first != 2 || strcmp(argv[first], "live") != 0 ||
        argv[first + 1][0] == '\0' || argv[first + 1][0] == '-') {
        return -1;
    }
    *next_name = argv[first + 1];
    return 0;
}

int qt_parse_relay(int argc, char *const argv[], int first,
                   const char **own_name, const char **next_name)
{
    int i;

    if (first < 1 || argc - first < 2 || argv[first][0] == '\0' ||
        argv[first][0] == '-' || argv[first + 1][0] == '\0' ||
        argv[first + 1][0] == '-') {
        return -1;
    }
    for (i = first + 2; i < argc; i++) {
        unsigned long ignored;

        if (strcmp(argv[i], "-r") == 0) {
            continue;
        }
        if (strcmp(argv[i], "-q") == 0 && i + 1 < argc &&
            parse_count(argv[i + 1], &ignored, 0) == 0) {
            i++;
            continue;
        }
        return -1;
    }
    *own_name = argv[first];
    *next_name = argv[first + 1];
    return 0;
}

int qt_parse_sink(int argc, char *const argv[], int first,
                  unsigned long *count, unsigned long *gap_ms)
{
    int i = first;
    unsigned long gap = QT_SINK_DEFAULT_GAP_MS;

    if (first < 1) {
        return -1;
    }
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc &&
            strcmp(argv[i + 1], "0") == 0) {
            i += 2;
        } else if (strcmp(argv[i], "-r") == 0) {
            i++;
        } else {
            return -1;
        }
    }
    if (argc - i != 1 && argc - i != 2) {
        return -1;
    }
    if (parse_count(argv[i], count, 0) != 0) {
        return -1;
    }
    if (argc - i == 2 && parse_count(argv[i + 1], &gap, 0) != 0) {
        return -1;
    }
    *gap_ms = gap;
    return 0;
}

int64_t qt_send_limit_ns(uint32_t class_id)
{
    return signal_class_deadline_ns(class_id);
}

int qt_line_send_timeout(char *buf, size_t cap, uint32_t class_id,
                         uint64_t number, int64_t after_ns)
{
    return fit(buf, cap, "source: send-timeout class %s seq %" PRIu64
               " after_ns %lld\n", signal_class_name(class_id), number,
               (long long)after_ns);
}

int qt_line_send_failed(char *buf, size_t cap, uint32_t class_id,
                        uint64_t number, int err)
{
    return fit(buf, cap, "source: send-failed class %s seq %" PRIu64
               " errno %d\n", signal_class_name(class_id), number, err);
}

int qt_line_person_plan(char *buf, size_t cap, int64_t start_ns,
                        int32_t start_eye_um, int64_t offset_ns,
                        int64_t event_ns, int64_t woke_ns,
                        double margin_m, double event_m)
{
    return fit(buf, cap,
               "source: person-plan approach_start_ns %lld start_eye_um %ld "
               "offset_ns %lld event_ns %lld woke_late_ns %lld margin_um %.0f "
               "event_eye_um %.0f\n",
               (long long)start_ns, (long)start_eye_um, (long long)offset_ns,
               (long long)event_ns, (long long)(woke_ns - event_ns),
               margin_m * 1e6, event_m * 1e6);
}

int qt_line_person_event(char *buf, size_t cap, int64_t event_ns,
                         uint64_t number)
{
    return fit(buf, cap, "source: person-in event_ns %lld URGENT %" PRIu64 "\n",
               (long long)event_ns, number);
}

int qt_line_sink_state(char *buf, size_t cap, const char *state, int quiet,
                       long long run_ms)
{
    if (state == NULL) {
        return -1;
    }
    if (quiet) {
        return fit(buf, cap, "sink: state %s at %lld ms\n", state, run_ms);
    }
    return fit(buf, cap, "sink: state %s\n", state);
}

int qt_line_sink_signal(char *buf, size_t cap, const struct signal *sig,
                        enum signal_verdict verdict, int64_t now_ns)
{
    int64_t prev;
    uint32_t hops;
    uint32_t i;
    size_t used;
    int n;

    if (sig == NULL) {
        return -1;
    }
    prev = sig->issued_ns;
    hops = sig->hop_count > SIGNAL_MAX_HOPS ? SIGNAL_MAX_HOPS : sig->hop_count;
    n = fit(buf, cap, "sink: signal %" PRIu64 " class %s %s, hops",
            sig->number, signal_class_name(sig->class_id),
            signal_verdict_name(verdict));
    if (n < 0) {
        return -1;
    }
    used = (size_t)n;
    for (i = 0; i < hops; i++) {
        n = fit(buf + used, cap - used, " %" PRId64 " ns,",
                sig->hop_ns[i] - prev);
        if (n < 0) {
            return -1;
        }
        used += (size_t)n;
        prev = sig->hop_ns[i];
    }
    n = fit(buf + used, cap - used, " %" PRId64 " ns, total %" PRId64 " ns\n",
            now_ns - prev, now_ns - sig->issued_ns);
    if (n < 0) {
        return -1;
    }
    return (int)(used + (size_t)n);
}

int qt_line_sink_late(char *buf, size_t cap, const struct signal *sig,
                      int64_t now_ns)
{
    if (sig == NULL) {
        return -1;
    }
    return fit(buf, cap, "sink: late signal %" PRIu64 " class %s at %lld ms, "
               "total %" PRId64 " ns\n", sig->number,
               signal_class_name(sig->class_id), (long long)(now_ns / 1000000LL),
               now_ns - sig->issued_ns);
}
