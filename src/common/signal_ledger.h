#ifndef SIGNAL_LEDGER_H
#define SIGNAL_LEDGER_H

/*
 * Every issued signal ends as on time, late, lost, or dropped.
 * No QNX headers. Numbers are 1..SIGNAL_LEDGER_CAP.
 * A drop is an issued BACKGROUND or COMMAND that has not arrived yet.
 * URGENT is never dropped.
 */

#include "signal_msg.h"

#include <stdint.h>

#define SIGNAL_LEDGER_CAP 3072

enum signal_account {
    SIGNAL_ACCOUNT_NONE = 0,
    SIGNAL_ACCOUNT_ON_TIME = 1,
    SIGNAL_ACCOUNT_LATE = 2,
    SIGNAL_ACCOUNT_LOST = 3,
    SIGNAL_ACCOUNT_DROPPED = 4
};

struct signal_ledger {
    uint8_t issued[SIGNAL_LEDGER_CAP + 1];
    uint8_t account[SIGNAL_LEDGER_CAP + 1];
    uint32_t class_id[SIGNAL_LEDGER_CAP + 1];
    unsigned issued_count;
};

void signal_ledger_init(struct signal_ledger *ledger);

/* 0 on success. -1 if the number is out of range or already issued. */
int signal_ledger_issue(struct signal_ledger *ledger, uint64_t number,
                        uint32_t class_id);

/*
 * 0 on success. -1 if the number is out of range or already counted
 * as on time or late. A number marked lost can still arrive; the
 * arrival replaces the loss.
 */
int signal_ledger_receive(struct signal_ledger *ledger, uint64_t number,
                          uint32_t class_id, enum signal_verdict verdict);

/* 0 if the number is now lost, or was already lost, or already arrived. */
int signal_ledger_lose(struct signal_ledger *ledger, uint64_t number,
                       uint32_t class_id);

/*
 * 0 when an issued, not-yet-arrived BACKGROUND or COMMAND is now
 * dropped. -1 if ledger is NULL, the number is out of range, the
 * number was not issued, the class is not BACKGROUND or COMMAND, or
 * it already arrived, was lost, or was dropped. URGENT is refused.
 * The account is unchanged on -1.
 */
int signal_ledger_drop(struct signal_ledger *ledger, uint64_t number);

/* Issued numbers that are still unaccounted. */
unsigned signal_ledger_unaccounted(const struct signal_ledger *ledger);

/*
 * Arrivals (on time or late) whose number was never on an issued list.
 * Lost and dropped do not count: a loss is not an arrival.
 */
unsigned signal_ledger_arrived_unissued(const struct signal_ledger *ledger);

#endif
