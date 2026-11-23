#ifndef ISSUED_LOG_H
#define ISSUED_LOG_H

/*
 * The lines "source: sent signal N class NAME" and the restart
 * number that follows them. No clock and no QNX headers.
 * A restarted source starts at max+2 so it does not reuse a number
 * the dead source may already have handed to the next node.
 */

#include "signal_ledger.h"
#include "wire.h"

#include <stddef.h>
#include <stdint.h>

#define ISSUED_LIST_PATH "/tmp/source-issued.txt"

struct issued_item {
    uint64_t number;
    uint32_t class_id;
};

/*
 * Parses complete lines. A trailing fragment with no newline is
 * ignored. 0 and *count / *max_number are set. -1 leaves them unchanged.
 * *max_number is 0 when *count is 0.
 */
int issued_list_parse(const char *text, struct issued_item *items,
                      unsigned cap, unsigned *count, uint64_t *max_number);

/* One log line, including the newline. 0, or -1 if it does not fit. */
int issued_list_line(const struct issued_item *item, char *buf, size_t cap);

/*
 * Next number for a restarted source. have_line == 0 ignores max_number
 * and yields 2. Otherwise max_number + 2, when that still fits in 1..CAP.
 * 0, or -1 and *next unchanged.
 */
int issued_list_restart_number(uint64_t max_number, int have_line,
                               uint64_t *next);

/*
 * prior items plus extra new signals fit in cap, and no prior number
 * is in the new range that starts at start. 0 and *total is the sum.
 * -1 leaves *total unchanged.
 */
int issued_list_fits(const struct issued_item *items, unsigned count,
                     uint64_t start, unsigned extra, unsigned cap,
                     unsigned *total);

int issued_list_is_rebuilt(uint32_t reserved);

/* The sentence sink prints when the manifest was rebuilt. */
const char *issued_list_rebuilt_sentence(void);

#endif
