#ifndef SIGNAL_LEDGER_H
#define SIGNAL_LEDGER_H

/*
 * Every issued signal ends as on time, late, lost, or dropped.
 * No QNX headers. Numbers start at 1 and have no upper limit: the
 * ledger is a ring of SIGNAL_LEDGER_CAP slots, slot = number % CAP.
 * A slot is reused by a newer number only once its number is settled
 * (issued and accounted). The live source never has more than CAP
 * numbers that are not closed yet, so a settled slot is always there
 * (D-062). Totals cover the whole run, not only the ring.
 * A drop is an issued BACKGROUND or COMMAND that has not arrived yet.
 * URGENT is never dropped.
 */

#include "signal_msg.h"

#include <stdint.h>

#define SIGNAL_LEDGER_CAP 12288

enum signal_account {
    SIGNAL_ACCOUNT_NONE = 0,
    SIGNAL_ACCOUNT_ON_TIME = 1,
    SIGNAL_ACCOUNT_LATE = 2,
    SIGNAL_ACCOUNT_LOST = 3,
    SIGNAL_ACCOUNT_DROPPED = 4
};

#define SIGNAL_ACCOUNT_KINDS 5

struct signal_ledger_slot {
    uint64_t number;
    uint32_t class_id;
    uint8_t issued;
    uint8_t account;
};

struct signal_ledger {
    struct signal_ledger_slot slot[SIGNAL_LEDGER_CAP];
    /* Whole-run totals by class (1..3) and account. Index 0 is unused. */
    unsigned count[4][SIGNAL_ACCOUNT_KINDS];
    unsigned issued_count;
    /* Issued and not accounted. */
    unsigned open_count;
    /* Arrived and not on any issued list yet. */
    unsigned unissued_count;
    /* Refused because the slot still held a newer or unsettled number. */
    unsigned window_refused;
};

void signal_ledger_init(struct signal_ledger *ledger);

/*
 * 0 on success. -1 if the number is 0, already issued, or its slot still
 * holds another number that is not settled (window_refused grows).
 */
int signal_ledger_issue(struct signal_ledger *ledger, uint64_t number,
                        uint32_t class_id);

/*
 * 0 on success. -1 if the number is 0, already counted as on time,
 * late, or dropped, or its slot is taken (window_refused grows). A
 * number marked lost can still arrive; the arrival replaces the loss.
 */
int signal_ledger_receive(struct signal_ledger *ledger, uint64_t number,
                          uint32_t class_id, enum signal_verdict verdict);

/*
 * 0 if the number is now lost, or was already lost, or already arrived.
 * -1 if the number is 0 or its slot is taken.
 */
int signal_ledger_lose(struct signal_ledger *ledger, uint64_t number,
                       uint32_t class_id);

/*
 * 0 when an issued, not-yet-arrived BACKGROUND or COMMAND is now
 * dropped. -1 if ledger is NULL, the number is not in the ring, the
 * number was not issued, the class is not BACKGROUND or COMMAND, or
 * it already arrived, was lost, or was dropped. URGENT is refused.
 * The account is unchanged on -1.
 */
int signal_ledger_drop(struct signal_ledger *ledger, uint64_t number);

/* The account of number, or SIGNAL_ACCOUNT_NONE if the ring does not hold it. */
enum signal_account signal_ledger_account(const struct signal_ledger *ledger,
                                          uint64_t number);

/* 1 if number is in the ring, issued, and still has no account. */
int signal_ledger_is_open(const struct signal_ledger *ledger, uint64_t number);

/* Whole-run totals for one class. Zeros for an unknown class. */
void signal_ledger_counts(const struct signal_ledger *ledger, uint32_t class_id,
                          unsigned *on_time, unsigned *late, unsigned *lost,
                          unsigned *dropped);

/* Issued numbers that are still unaccounted. */
unsigned signal_ledger_unaccounted(const struct signal_ledger *ledger);

/*
 * Arrivals (on time or late) whose number was never on an issued list.
 * Lost and dropped do not count: a loss is not an arrival.
 */
unsigned signal_ledger_arrived_unissued(const struct signal_ledger *ledger);

#endif
