#include "signal_ledger.h"

#include <stddef.h>
#include <string.h>

void signal_ledger_init(struct signal_ledger *ledger)
{
    if (ledger == NULL) {
        return;
    }
    memset(ledger, 0, sizeof *ledger);
}

static int class_ok(uint32_t class_id)
{
    return class_id >= SIGNAL_CLASS_URGENT && class_id <= SIGNAL_CLASS_BACKGROUND;
}

static void count_add(struct signal_ledger *ledger, uint32_t class_id,
                      uint8_t account, int delta)
{
    if (!class_ok(class_id) || account == SIGNAL_ACCOUNT_NONE ||
        account >= SIGNAL_ACCOUNT_KINDS) {
        return;
    }
    if (delta < 0) {
        if (ledger->count[class_id][account] > 0) {
            ledger->count[class_id][account]--;
        }
        return;
    }
    ledger->count[class_id][account]++;
}

static int settled(const struct signal_ledger_slot *slot)
{
    return slot->issued && slot->account != SIGNAL_ACCOUNT_NONE;
}

/* The slot that holds number, or NULL. */
static struct signal_ledger_slot *find(const struct signal_ledger *ledger,
                                       uint64_t number)
{
    const struct signal_ledger_slot *slot;

    if (ledger == NULL || number == 0) {
        return NULL;
    }
    slot = &ledger->slot[number % SIGNAL_LEDGER_CAP];
    return slot->number == number ? (struct signal_ledger_slot *)slot : NULL;
}

/*
 * The slot for number, taking it when it is empty or holds an older
 * settled number. NULL when the slot is still in use (D-062).
 */
static struct signal_ledger_slot *claim(struct signal_ledger *ledger,
                                        uint64_t number)
{
    struct signal_ledger_slot *slot;

    if (ledger == NULL || number == 0) {
        return NULL;
    }
    slot = &ledger->slot[number % SIGNAL_LEDGER_CAP];
    if (slot->number == number) {
        return slot;
    }
    if (slot->number == 0 || (slot->number < number && settled(slot))) {
        memset(slot, 0, sizeof *slot);
        slot->number = number;
        return slot;
    }
    ledger->window_refused++;
    return NULL;
}

int signal_ledger_issue(struct signal_ledger *ledger, uint64_t number,
                        uint32_t class_id)
{
    struct signal_ledger_slot *slot = claim(ledger, number);

    if (slot == NULL || slot->issued) {
        return -1;
    }
    slot->issued = 1;
    ledger->issued_count++;
    if (slot->account == SIGNAL_ACCOUNT_NONE) {
        slot->class_id = class_id;
        ledger->open_count++;
    } else if (slot->account == SIGNAL_ACCOUNT_ON_TIME ||
               slot->account == SIGNAL_ACCOUNT_LATE) {
        if (ledger->unissued_count > 0) {
            ledger->unissued_count--;
        }
    }
    return 0;
}

int signal_ledger_receive(struct signal_ledger *ledger, uint64_t number,
                          uint32_t class_id, enum signal_verdict verdict)
{
    struct signal_ledger_slot *slot = claim(ledger, number);
    uint8_t next;

    if (slot == NULL) {
        return -1;
    }
    if (slot->account == SIGNAL_ACCOUNT_ON_TIME ||
        slot->account == SIGNAL_ACCOUNT_LATE ||
        slot->account == SIGNAL_ACCOUNT_DROPPED) {
        return -1;
    }
    if (slot->account == SIGNAL_ACCOUNT_LOST) {
        count_add(ledger, slot->class_id, SIGNAL_ACCOUNT_LOST, -1);
    } else if (slot->issued) {
        if (ledger->open_count > 0) {
            ledger->open_count--;
        }
    } else {
        ledger->unissued_count++;
    }
    next = verdict == SIGNAL_ON_TIME ? SIGNAL_ACCOUNT_ON_TIME
                                     : SIGNAL_ACCOUNT_LATE;
    slot->account = next;
    slot->class_id = class_id;
    count_add(ledger, class_id, next, 1);
    return 0;
}

int signal_ledger_lose(struct signal_ledger *ledger, uint64_t number,
                       uint32_t class_id)
{
    struct signal_ledger_slot *slot = claim(ledger, number);

    if (slot == NULL) {
        return -1;
    }
    if (slot->account != SIGNAL_ACCOUNT_NONE) {
        return 0;
    }
    if (slot->issued && ledger->open_count > 0) {
        ledger->open_count--;
    }
    slot->account = SIGNAL_ACCOUNT_LOST;
    slot->class_id = class_id;
    count_add(ledger, class_id, SIGNAL_ACCOUNT_LOST, 1);
    return 0;
}

int signal_ledger_drop(struct signal_ledger *ledger, uint64_t number)
{
    struct signal_ledger_slot *slot = find(ledger, number);

    if (slot == NULL || !slot->issued) {
        return -1;
    }
    if (slot->class_id != SIGNAL_CLASS_BACKGROUND &&
        slot->class_id != SIGNAL_CLASS_COMMAND) {
        return -1;
    }
    if (slot->account != SIGNAL_ACCOUNT_NONE) {
        return -1;
    }
    slot->account = SIGNAL_ACCOUNT_DROPPED;
    if (ledger->open_count > 0) {
        ledger->open_count--;
    }
    count_add(ledger, slot->class_id, SIGNAL_ACCOUNT_DROPPED, 1);
    return 0;
}

enum signal_account signal_ledger_account(const struct signal_ledger *ledger,
                                          uint64_t number)
{
    const struct signal_ledger_slot *slot = find(ledger, number);

    return slot == NULL ? SIGNAL_ACCOUNT_NONE : (enum signal_account)slot->account;
}

int signal_ledger_is_open(const struct signal_ledger *ledger, uint64_t number)
{
    const struct signal_ledger_slot *slot = find(ledger, number);

    return slot != NULL && slot->issued && slot->account == SIGNAL_ACCOUNT_NONE;
}

void signal_ledger_counts(const struct signal_ledger *ledger, uint32_t class_id,
                          unsigned *on_time, unsigned *late, unsigned *lost,
                          unsigned *dropped)
{
    unsigned zero[SIGNAL_ACCOUNT_KINDS] = {0, 0, 0, 0, 0};
    const unsigned *row = zero;

    if (ledger != NULL && class_ok(class_id)) {
        row = ledger->count[class_id];
    }
    if (on_time != NULL) {
        *on_time = row[SIGNAL_ACCOUNT_ON_TIME];
    }
    if (late != NULL) {
        *late = row[SIGNAL_ACCOUNT_LATE];
    }
    if (lost != NULL) {
        *lost = row[SIGNAL_ACCOUNT_LOST];
    }
    if (dropped != NULL) {
        *dropped = row[SIGNAL_ACCOUNT_DROPPED];
    }
}

unsigned signal_ledger_arrived_unissued(const struct signal_ledger *ledger)
{
    return ledger == NULL ? 0 : ledger->unissued_count;
}

unsigned signal_ledger_unaccounted(const struct signal_ledger *ledger)
{
    return ledger == NULL ? 0 : ledger->open_count;
}
