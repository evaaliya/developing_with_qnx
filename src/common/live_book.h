#ifndef LIVE_BOOK_H
#define LIVE_BOOK_H

/*
 * The live source's books (D-063): the ring of numbers not closed yet,
 * the next number, and the numbers each class has closed. The book
 * lives in shared memory, so a source the supervisor restarts goes on
 * from the dead one's numbers. No QNX headers and no clock.
 *
 * A SIGKILL can land between any two stores. A number goes on the
 * ring with one commit (the ring count is written after the item), and
 * the next number is written after that. Removing closed numbers
 * changes the head, the count, and the closed totals together, so it
 * is written to a journal first and redone on resume. The mode (flood,
 * mute) is one 32-bit word, so it too has one commit (D-065).
 */

#include "close_list.h"

#include <stdint.h>

#define LIVE_BOOK_MAGIC 0x4C424B31u
/* 2: the mode word (D-065). A version 1 book is not resumed. */
#define LIVE_BOOK_VERSION 2u
#define LIVE_BOOK_STORE "/signal-source-book"

/* The mode word: SET once a source wrote it, then flood and mute. */
#define LIVE_BOOK_MODE_SET 0x100u
#define LIVE_BOOK_MODE_FLOOD 0x1u
#define LIVE_BOOK_MODE_MUTE 0x2u

struct live_book_journal {
    uint32_t valid;
    uint32_t head;
    uint32_t count;
    uint32_t reserved;
    uint64_t closed[4];
};

struct live_book {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    /* How many restarted sources took this book over. */
    uint32_t resumes;
    uint64_t next;
    /* Numbers each class (index = class id) has closed and left the ring. */
    uint64_t closed[4];
    /* LIVE_BOOK_MODE_*: the mode the source was in when it last changed. */
    uint32_t mode;
    uint32_t mode_reserved;
    struct live_book_journal journal;
    struct issued_ring ring;
};

struct live_book_resume {
    uint64_t next;
    unsigned waiting;
    uint64_t issued[4];
    /* 1 when a close was cut short by the death and was finished now. */
    int journal_redone;
    /* 1 when the dead source died after putting a number on the ring
       and before moving next past it. */
    int next_moved;
    /* 1 when the book held a mode the dead source wrote; then flood and
       urgent_on are that mode. */
    int mode_known;
    int flood;
    int urgent_on;
};

/* Fresh books: numbers start at 1. */
void live_book_init(struct live_book *book);

/*
 * Puts the next number on the ring before it is sent, so a death in
 * the send leaves it to be closed. 0 and *number, or -1 when the ring
 * is full (closes stopped coming back) or an argument is bad.
 */
int live_book_take(struct live_book *book, uint32_t class_id, int64_t now_ns,
                   uint64_t *number);

/* The source changed mode: one store, so a death leaves the old or the new. */
void live_book_set_mode(struct live_book *book, int flood, int urgent_on);

/* The n oldest numbers were closed: count them by class and drop them. */
void live_book_close_front(struct live_book *book, unsigned n);

/* Numbers of one class issued over the whole run. */
uint64_t live_book_issued(const struct live_book *book, uint32_t class_id);

/*
 * Takes over the book of a dead source. -1 when book is not a whole
 * live book (magic, version, size, ring bounds, classes, or numbers out
 * of order); the caller then starts new books and says so. On 0, *out
 * says where the numbers go on, and resumes has grown by one.
 */
int live_book_resume(struct live_book *book, struct live_book_resume *out);

#endif
