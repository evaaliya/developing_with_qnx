#ifndef DROP_BOOK_H
#define DROP_BOOK_H

/*
 * Numbers a relay dropped that no close or manifest has carried yet
 * (D-062). The list lives in shared memory, so a relay the supervisor
 * restarts still marks them dropped instead of letting sink count them
 * lost (D-063). No QNX headers.
 *
 * Adding writes the number, then the count. Forgetting compacts the
 * array forward and then writes the count, so a death in the middle
 * may leave a forgotten number behind, never lose one that is kept.
 * A leftover number is harmless: closes carry only numbers not closed.
 */

#include "wire.h"

#include <stdint.h>

#define DROP_BOOK_MAGIC 0x44424B31u
#define DROP_BOOK_VERSION 1u

struct drop_book {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    /* How many restarted relays took this list over. */
    uint32_t resumes;
    uint32_t count;
    uint32_t reserved;
    uint64_t number[SIGNAL_MANIFEST_CAP];
};

void drop_book_init(struct drop_book *book);

/* 0, or -1 when the list is full or book is NULL. */
int drop_book_add(struct drop_book *book, uint64_t number);

/* Removes every number in lo..hi. */
void drop_book_forget(struct drop_book *book, uint64_t lo, uint64_t hi);

/* 0 and resumes grows by one, or -1 when book is not a whole list. */
int drop_book_resume(struct drop_book *book);

/* The shared memory name for a relay attached as own_name. 0 or -1. */
int drop_book_store_name(const char *own_name, char *buf, unsigned cap);

#endif
