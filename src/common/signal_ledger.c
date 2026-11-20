#include "signal_ledger.h"

#include <stddef.h>
#include <string.h>

static int in_range(uint64_t number)
{
    return number >= 1 && number <= SIGNAL_LEDGER_CAP;
}

void signal_ledger_init(struct signal_ledger *ledger)
{
    if (ledger == NULL) {
        return;
    }
    memset(ledger, 0, sizeof *ledger);
}

int signal_ledger_issue(struct signal_ledger *ledger, uint64_t number,
                        uint32_t class_id)
{
    if (ledger == NULL || !in_range(number) || ledger->issued[number]) {
        return -1;
    }
    ledger->issued[number] = 1;
    ledger->issued_count++;
    if (ledger->account[number] == SIGNAL_ACCOUNT_NONE) {
        ledger->class_id[number] = class_id;
    }
    return 0;
}

int signal_ledger_receive(struct signal_ledger *ledger, uint64_t number,
                          uint32_t class_id, enum signal_verdict verdict)
{
    uint8_t next;

    if (ledger == NULL || !in_range(number)) {
        return -1;
    }
    if (ledger->account[number] == SIGNAL_ACCOUNT_ON_TIME ||
        ledger->account[number] == SIGNAL_ACCOUNT_LATE) {
        return -1;
    }
    next = verdict == SIGNAL_ON_TIME ? SIGNAL_ACCOUNT_ON_TIME
                                     : SIGNAL_ACCOUNT_LATE;
    ledger->account[number] = next;
    ledger->class_id[number] = class_id;
    return 0;
}

int signal_ledger_lose(struct signal_ledger *ledger, uint64_t number,
                       uint32_t class_id)
{
    if (ledger == NULL || !in_range(number)) {
        return -1;
    }
    if (ledger->account[number] == SIGNAL_ACCOUNT_ON_TIME ||
        ledger->account[number] == SIGNAL_ACCOUNT_LATE ||
        ledger->account[number] == SIGNAL_ACCOUNT_LOST) {
        return 0;
    }
    ledger->account[number] = SIGNAL_ACCOUNT_LOST;
    ledger->class_id[number] = class_id;
    return 0;
}

unsigned signal_ledger_unaccounted(const struct signal_ledger *ledger)
{
    unsigned missing = 0;
    unsigned number;

    if (ledger == NULL) {
        return 0;
    }
    for (number = 1; number <= SIGNAL_LEDGER_CAP; number++) {
        if (ledger->issued[number] &&
            ledger->account[number] == SIGNAL_ACCOUNT_NONE) {
            missing++;
        }
    }
    return missing;
}
