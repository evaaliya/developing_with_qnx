#include "drop_notes.h"

#include "signal_msg.h"

#include <string.h>

void drop_notes_init(struct drop_notes *notes)
{
    if (notes != NULL) {
        memset(notes, 0, sizeof *notes);
    }
}

static int class_ok(uint32_t class_id)
{
    return class_id == SIGNAL_CLASS_URGENT ||
           class_id == SIGNAL_CLASS_COMMAND ||
           class_id == SIGNAL_CLASS_BACKGROUND;
}

int drop_notes_add(struct drop_notes *notes, uint64_t number,
                   uint32_t class_id, int64_t at_ns)
{
    unsigned slot;

    if (notes == NULL || !class_ok(class_id)) {
        return -1;
    }
    if (notes->count >= DROP_NOTES_CAP) {
        notes->missed++;
        return -1;
    }
    slot = (notes->head + notes->count) % DROP_NOTES_CAP;
    notes->item[slot].number = number;
    notes->item[slot].class_id = class_id;
    notes->item[slot].at_ns = at_ns;
    notes->count++;
    return 0;
}

int drop_notes_want_announce(struct drop_notes *notes, uint32_t class_id,
                             uint64_t number)
{
    unsigned bit;

    if (notes == NULL || !class_ok(class_id)) {
        return -1;
    }
    bit = 1u << (class_id - 1);
    if ((notes->announce & bit) == 0) {
        notes->announce_number[class_id - 1] = number;
    }
    notes->announce |= bit;
    return 0;
}

int drop_notes_pending(const struct drop_notes *notes)
{
    if (notes == NULL) {
        return 0;
    }
    return notes->count > 0 || notes->announce != 0 || notes->missed > 0;
}

unsigned drop_notes_take(struct drop_notes *notes, struct drop_note *out,
                         unsigned max, unsigned long *missed)
{
    unsigned n = 0;

    if (missed != NULL) {
        *missed = 0;
    }
    if (notes == NULL) {
        return 0;
    }
    if (missed != NULL) {
        *missed = notes->missed;
        notes->missed = 0;
    }
    if (out == NULL) {
        return 0;
    }
    while (n < max && notes->count > 0) {
        out[n++] = notes->item[notes->head];
        notes->head = (notes->head + 1) % DROP_NOTES_CAP;
        notes->count--;
    }
    return n;
}

unsigned drop_notes_take_announce(struct drop_notes *notes, uint64_t *numbers)
{
    unsigned bits;
    unsigned i;

    if (notes == NULL) {
        return 0;
    }
    bits = notes->announce;
    for (i = 0; i < 3; i++) {
        if (numbers != NULL) {
            numbers[i] = (bits & (1u << i)) ? notes->announce_number[i] : 0;
        }
        notes->announce_number[i] = 0;
    }
    notes->announce = 0;
    return bits;
}
