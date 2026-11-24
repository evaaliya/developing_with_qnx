#include "signal_ledger.h"

#include <stddef.h>
#include <string.h>

void signal_ledger_init(struct signal_ledger *ledger)
{
    if (ledger == NULL) {
        return;
    }
    memset(ledger, 0, sizeof *ledger);
    ledger->magic = SIGNAL_LEDGER_MAGIC;
    ledger->version = SIGNAL_LEDGER_VERSION;
    ledger->size = (uint32_t)sizeof *ledger;
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

static void slot_clear(struct signal_ledger_slot *slot, uint64_t number)
{
    slot->account = SIGNAL_ACCOUNT_NONE;
    slot->issued = 0;
    slot->class_id = 0;
    __atomic_store_n(&slot->number, number, __ATOMIC_RELEASE);
}

/*
 * A newer number takes a settled slot. The old number's account moves
 * to the retired totals and the slot is cleared: two writes, so the
 * result goes to the journal first and resume redoes it (D-063).
 */
static void hand_over(struct signal_ledger *ledger,
                      struct signal_ledger_slot *slot, uint64_t number)
{
    struct signal_ledger_journal *j = &ledger->journal;

    memcpy(j->retired, ledger->retired, sizeof j->retired);
    if (class_ok(slot->class_id) && slot->account != SIGNAL_ACCOUNT_NONE &&
        slot->account < SIGNAL_ACCOUNT_KINDS) {
        j->retired[slot->class_id][slot->account]++;
    }
    j->retired_issued = ledger->retired_issued + (slot->issued ? 1u : 0u);
    j->index = (uint32_t)(slot - ledger->slot);
    j->number = number;
    __atomic_store_n(&j->valid, 1u, __ATOMIC_RELEASE);
    memcpy(ledger->retired, j->retired, sizeof ledger->retired);
    ledger->retired_issued = j->retired_issued;
    slot_clear(slot, number);
    __atomic_store_n(&j->valid, 0u, __ATOMIC_RELEASE);
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
    if (slot->number == 0) {
        /* An empty slot: the number is the one write. */
        __atomic_store_n(&slot->number, number, __ATOMIC_RELEASE);
        return slot;
    }
    if (slot->number < number && settled(slot)) {
        hand_over(ledger, slot, number);
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
    /* The class before the flag: a death between them leaves the
       number not issued, which resume counts as such (D-063). */
    if (slot->account == SIGNAL_ACCOUNT_NONE) {
        slot->class_id = class_id;
    }
    __atomic_store_n(&slot->issued, (uint8_t)1, __ATOMIC_RELEASE);
    ledger->issued_count++;
    if (slot->account == SIGNAL_ACCOUNT_NONE) {
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
    slot->class_id = class_id;
    __atomic_store_n(&slot->account, next, __ATOMIC_RELEASE);
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
    slot->class_id = class_id;
    __atomic_store_n(&slot->account, (uint8_t)SIGNAL_ACCOUNT_LOST,
                     __ATOMIC_RELEASE);
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
    __atomic_store_n(&slot->account, (uint8_t)SIGNAL_ACCOUNT_DROPPED,
                     __ATOMIC_RELEASE);
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

int signal_ledger_is_issued(const struct signal_ledger *ledger, uint64_t number)
{
    const struct signal_ledger_slot *slot = find(ledger, number);

    return slot != NULL && slot->issued;
}

int signal_ledger_resume(struct signal_ledger *ledger,
                         struct signal_ledger_resume *out)
{
    unsigned i;

    if (ledger == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (ledger->magic != SIGNAL_LEDGER_MAGIC ||
        ledger->version != SIGNAL_LEDGER_VERSION ||
        ledger->size != (uint32_t)sizeof *ledger) {
        return -1;
    }
    if (ledger->journal.valid) {
        struct signal_ledger_journal *j = &ledger->journal;

        if (j->index >= SIGNAL_LEDGER_CAP || j->number == 0 ||
            j->number % SIGNAL_LEDGER_CAP != j->index) {
            return -1;
        }
        memcpy(ledger->retired, j->retired, sizeof ledger->retired);
        ledger->retired_issued = j->retired_issued;
        slot_clear(&ledger->slot[j->index], j->number);
        j->valid = 0;
        out->journal_redone = 1;
    }
    for (i = 0; i < SIGNAL_LEDGER_CAP; i++) {
        const struct signal_ledger_slot *slot = &ledger->slot[i];

        if (slot->number == 0) {
            continue;
        }
        if (slot->number % SIGNAL_LEDGER_CAP != i ||
            slot->account >= SIGNAL_ACCOUNT_KINDS || slot->issued > 1 ||
            (slot->account != SIGNAL_ACCOUNT_NONE && !class_ok(slot->class_id)) ||
            (slot->issued && !class_ok(slot->class_id))) {
            return -1;
        }
    }
    /* Every total again: the retired part plus what the slots hold. */
    memcpy(ledger->count, ledger->retired, sizeof ledger->count);
    ledger->issued_count = ledger->retired_issued;
    ledger->open_count = 0;
    ledger->unissued_count = 0;
    for (i = 0; i < SIGNAL_LEDGER_CAP; i++) {
        const struct signal_ledger_slot *slot = &ledger->slot[i];

        if (slot->number == 0) {
            continue;
        }
        out->slots_used++;
        if (slot->issued) {
            ledger->issued_count++;
        }
        if (slot->account == SIGNAL_ACCOUNT_NONE) {
            if (slot->issued) {
                ledger->open_count++;
            }
            continue;
        }
        ledger->count[slot->class_id][slot->account]++;
        if (!slot->issued && (slot->account == SIGNAL_ACCOUNT_ON_TIME ||
                              slot->account == SIGNAL_ACCOUNT_LATE)) {
            ledger->unissued_count++;
        }
    }
    ledger->resumes++;
    return 0;
}

int signal_ledger_close(struct signal_ledger *ledger,
                        const struct signal_manifest_item *items, unsigned n,
                        void (*on_lost)(uint64_t number, uint32_t class_id,
                                        void *arg),
                        void *arg, struct signal_ledger_close_result *out)
{
    static uint8_t fresh[SIGNAL_MANIFEST_CAP];
    unsigned i;

    if (ledger == NULL || out == NULL || (items == NULL && n > 0) ||
        n > SIGNAL_MANIFEST_CAP) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    for (i = 0; i < n; i++) {
        fresh[i] = !signal_ledger_is_issued(ledger, items[i].number);
        if (!fresh[i]) {
            out->repeated++;
            continue;
        }
        if (signal_ledger_issue(ledger, items[i].number,
                                items[i].class_id) != 0) {
            out->refused = items[i].number;
            return -1;
        }
    }
    for (i = 0; i < n; i++) {
        if (!fresh[i] || items[i].reserved != SIGNAL_MANIFEST_DROPPED) {
            continue;
        }
        if (signal_ledger_drop(ledger, items[i].number) != 0) {
            /* Arrived, or URGENT: the account it has stands. */
            out->drop_refused++;
            continue;
        }
        out->dropped++;
    }
    for (i = 0; i < n; i++) {
        if (!fresh[i] || !signal_ledger_is_open(ledger, items[i].number)) {
            continue;
        }
        if (signal_ledger_lose(ledger, items[i].number,
                               items[i].class_id) != 0) {
            out->refused = items[i].number;
            return -1;
        }
        if (on_lost != NULL) {
            on_lost(items[i].number, items[i].class_id, arg);
        }
        out->lost++;
    }
    return 0;
}
