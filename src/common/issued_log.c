#include "issued_log.h"

#include "signal_msg.h"

#include <stdio.h>
#include <string.h>

static int starts_with(const char *line, const char *end, const char *prefix)
{
    size_t len = strlen(prefix);

    if ((size_t)(end - line) < len) {
        return 0;
    }
    return memcmp(line, prefix, len) == 0;
}

static int parse_send_line(const char *line, const char *end,
                           struct issued_item *item)
{
    const char *prefix = "source: sent signal ";
    const char *p;
    uint64_t number = 0;
    size_t name_len;

    if (!starts_with(line, end, prefix)) {
        return 1;
    }
    p = line + strlen(prefix);
    if (p >= end || *p < '0' || *p > '9') {
        return -1;
    }
    while (p < end && *p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');

        if (number > (UINT64_MAX - digit) / 10u) {
            return -1;
        }
        number = number * 10u + digit;
        p++;
    }
    if (number < 1 || number > SIGNAL_LEDGER_CAP) {
        return -1;
    }
    if (!starts_with(p, end, " class ")) {
        return -1;
    }
    p += strlen(" class ");
    name_len = (size_t)(end - p);
    if (name_len == strlen("URGENT") && memcmp(p, "URGENT", name_len) == 0) {
        item->class_id = SIGNAL_CLASS_URGENT;
    } else if (name_len == strlen("COMMAND") &&
               memcmp(p, "COMMAND", name_len) == 0) {
        item->class_id = SIGNAL_CLASS_COMMAND;
    } else if (name_len == strlen("BACKGROUND") &&
               memcmp(p, "BACKGROUND", name_len) == 0) {
        item->class_id = SIGNAL_CLASS_BACKGROUND;
    } else {
        return -1;
    }
    item->number = number;
    return 0;
}

int issued_list_parse(const char *text, struct issued_item *items,
                      unsigned cap, unsigned *count, uint64_t *max_number)
{
    static struct issued_item local[SIGNAL_LEDGER_CAP];
    unsigned local_count = 0;
    uint64_t max_seen = 0;
    const char *p;

    if (text == NULL || items == NULL || count == NULL || max_number == NULL) {
        return -1;
    }

    p = text;
    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        const char *end;
        struct issued_item item;
        int rc;
        unsigned i;

        if (nl == NULL) {
            break;
        }
        end = nl;
        if (end > p && end[-1] == '\r') {
            end--;
        }
        rc = parse_send_line(p, end, &item);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            if (local_count >= SIGNAL_LEDGER_CAP) {
                return -1;
            }
            for (i = 0; i < local_count; i++) {
                if (local[i].number == item.number) {
                    return -1;
                }
            }
            local[local_count++] = item;
            if (item.number > max_seen) {
                max_seen = item.number;
            }
        }
        p = nl + 1;
    }

    if (local_count > cap) {
        return -1;
    }
    if (local_count > 0) {
        memcpy(items, local, local_count * sizeof local[0]);
    }
    *count = local_count;
    *max_number = max_seen;
    return 0;
}

int issued_list_line(const struct issued_item *item, char *buf, size_t cap)
{
    const char *name;
    int wrote;

    if (item == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    name = signal_class_name(item->class_id);
    if (strcmp(name, "unknown") == 0) {
        return -1;
    }
    wrote = snprintf(buf, cap, "source: sent signal %llu class %s\n",
                     (unsigned long long)item->number, name);
    if (wrote < 0 || (size_t)wrote >= cap) {
        return -1;
    }
    return 0;
}

int issued_list_restart_number(uint64_t max_number, int have_line,
                               uint64_t *next)
{
    uint64_t base;

    if (next == NULL) {
        return -1;
    }
    if (!have_line) {
        base = 0;
    } else if (max_number < 1 || max_number > SIGNAL_LEDGER_CAP) {
        return -1;
    } else {
        base = max_number;
    }
    if (base > (uint64_t)SIGNAL_LEDGER_CAP - 2u) {
        return -1;
    }
    *next = base + 2u;
    return 0;
}

int issued_list_fits(const struct issued_item *items, unsigned count,
                     uint64_t start, unsigned extra, unsigned cap,
                     unsigned *total)
{
    unsigned i;

    if (total == NULL || extra > cap || count > cap - extra) {
        return -1;
    }
    if (count > 0 && items == NULL) {
        return -1;
    }
    if (extra > 0) {
        if (start < 1 || start > cap || start > cap - (extra - 1u)) {
            return -1;
        }
    }
    for (i = 0; i < count; i++) {
        if (items[i].number < 1 || items[i].number > cap) {
            return -1;
        }
        if (extra > 0 && items[i].number >= start) {
            return -1;
        }
    }
    *total = count + extra;
    return 0;
}

int issued_list_is_rebuilt(uint32_t reserved)
{
    return reserved == SIGNAL_MANIFEST_REBUILT;
}

const char *issued_list_rebuilt_sentence(void)
{
    return "an earlier source died before its issued list arrived; "
           "this list was rebuilt from the lines that source printed, "
           "and a signal with no printed line is not in this summary";
}
