#ifndef DROP_NOTES_H
#define DROP_NOTES_H

/*
 * Work the relay receive thread hands to its notice thread (D-069):
 * the log lines of dropped signals, and drop notices to pass to the
 * next node. The receive thread only records; printing and the
 * MsgSend of the notice happen on the notice thread, so the receive
 * thread is back in MsgReceive for the next URGENT at once.
 * No locking here: the caller holds its own mutex.
 */

#include <stdint.h>

#define DROP_NOTES_CAP 1024u

struct drop_note {
    uint64_t number;
    uint32_t class_id;
    int64_t at_ns;
};

struct drop_notes {
    struct drop_note item[DROP_NOTES_CAP];
    unsigned head;
    unsigned count;
    /* Lines not kept because the ring was full. */
    unsigned long missed;
    /* One bit per class (bit class_id - 1) with a notice still to send. */
    unsigned announce;
    /* The first dropped number of each class waiting for its notice. */
    uint64_t announce_number[3];
};

void drop_notes_init(struct drop_notes *notes);

/* 0 when kept, -1 when the ring is full (counted in missed) or bad input. */
int drop_notes_add(struct drop_notes *notes, uint64_t number,
                   uint32_t class_id, int64_t at_ns);

/*
 * A drop notice of this class is to be sent. Notices of one class that
 * wait together become one: the next node uses only the class.
 * 0, or -1 for a class that is not one of the three.
 */
int drop_notes_want_announce(struct drop_notes *notes, uint32_t class_id,
                             uint64_t number);

/* 1 when lines or notices wait. */
int drop_notes_pending(const struct drop_notes *notes);

/*
 * Moves up to max lines, oldest first, into out and returns how many.
 * *missed gets the lines lost to a full ring since the last take and
 * the count is reset.
 */
unsigned drop_notes_take(struct drop_notes *notes, struct drop_note *out,
                         unsigned max, unsigned long *missed);

/*
 * The classes with a notice to send (bit class_id - 1); clears them.
 * numbers (3 entries, may be NULL) gets the first number of each class.
 */
unsigned drop_notes_take_announce(struct drop_notes *notes, uint64_t *numbers);

#endif
