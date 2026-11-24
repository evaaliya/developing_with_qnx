#include "drop_book.h"

#include "close_list.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void drop_book_init(struct drop_book *book)
{
    if (book == NULL) {
        return;
    }
    memset(book, 0, sizeof *book);
    book->magic = DROP_BOOK_MAGIC;
    book->version = DROP_BOOK_VERSION;
    book->size = (uint32_t)sizeof *book;
}

int drop_book_add(struct drop_book *book, uint64_t number)
{
    if (book == NULL || book->count >= SIGNAL_MANIFEST_CAP) {
        return -1;
    }
    book->number[book->count] = number;
    __atomic_store_n(&book->count, book->count + 1, __ATOMIC_RELEASE);
    return 0;
}

void drop_book_forget(struct drop_book *book, uint64_t lo, uint64_t hi)
{
    unsigned kept;

    if (book == NULL) {
        return;
    }
    kept = close_forget_dropped(book->number, book->count, lo, hi);
    __atomic_store_n(&book->count, (uint32_t)kept, __ATOMIC_RELEASE);
}

int drop_book_resume(struct drop_book *book)
{
    if (book == NULL || book->magic != DROP_BOOK_MAGIC ||
        book->version != DROP_BOOK_VERSION ||
        book->size != (uint32_t)sizeof *book ||
        book->count > SIGNAL_MANIFEST_CAP) {
        return -1;
    }
    book->resumes++;
    return 0;
}

int drop_book_store_name(const char *own_name, char *buf, unsigned cap)
{
    int n;

    if (own_name == NULL || own_name[0] == '\0' || buf == NULL ||
        strchr(own_name, '/') != NULL) {
        return -1;
    }
    n = snprintf(buf, cap, "/%s-drops", own_name);
    if (n < 0 || (unsigned)n >= cap) {
        return -1;
    }
    return 0;
}
