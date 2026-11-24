#ifndef NODE_STORE_H
#define NODE_STORE_H

/*
 * A node's books in a named POSIX shared memory object (D-063). The
 * object outlives the process until it is unlinked, so a node that the
 * supervisor restarts after SIGKILL finds the books of the dead one.
 * POSIX only: the host tests use the same code.
 */

#include <stddef.h>

enum node_store_found {
    /* A new, zeroed object: the caller starts fresh books. */
    NODE_STORE_NEW = 0,
    /* resume was asked and an object of the right size was there. */
    NODE_STORE_KEPT = 1,
    /* resume was asked and there was no object: books before are gone. */
    NODE_STORE_MISSING = 2,
    /* resume was asked and the object was too short: replaced. */
    NODE_STORE_WRONG_SIZE = 3
};

/*
 * Maps name (a leading '/', no other '/') read-write and shared.
 * resume 0 removes any old object and makes a new zeroed one.
 * resume 1 keeps an existing object of at least size bytes.
 * NULL with errno set on failure; *found tells what was there.
 */
void *node_store_map(const char *name, size_t size, int resume,
                     enum node_store_found *found);

/* 0, or -1 with errno set. */
int node_store_unmap(void *base, size_t size);

/* Removes the object. 0 when it is gone or was not there. */
int node_store_remove(const char *name);

/* A short English phrase for found, for the node's restart line. */
const char *node_store_found_text(enum node_store_found found);

#endif
