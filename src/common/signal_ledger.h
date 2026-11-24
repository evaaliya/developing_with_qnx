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
#include "wire.h"

#include <stdint.h>

#define SIGNAL_LEDGER_CAP 12288

/* The sink keeps its ledger in shared memory under this name (D-063). */
#define SIGNAL_LEDGER_MAGIC 0x53424B31u
#define SIGNAL_LEDGER_VERSION 1u
#define SIGNAL_LEDGER_STORE "/signal-sink-book"

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

/*
 * A slot taken by a newer number moves the old number's account into
 * the retired totals. That is two writes, so it goes through a journal
 * that resume redoes (D-063).
 */
struct signal_ledger_journal {
    uint32_t valid;
    uint32_t index;
    uint64_t number;
    unsigned retired[4][SIGNAL_ACCOUNT_KINDS];
    unsigned retired_issued;
};

struct signal_ledger {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    /* How many restarted sinks took this ledger over. */
    uint32_t resumes;
    struct signal_ledger_slot slot[SIGNAL_LEDGER_CAP];
    /* Whole-run totals by class (1..3) and account. Index 0 is unused. */
    unsigned count[4][SIGNAL_ACCOUNT_KINDS];
    /* The part of count whose slots newer numbers have taken. */
    unsigned retired[4][SIGNAL_ACCOUNT_KINDS];
    unsigned retired_issued;
    struct signal_ledger_journal journal;
    unsigned issued_count;
    /* Issued and not accounted. */
    unsigned open_count;
    /* Arrived and not on any issued list yet. */
    unsigned unissued_count;
    /* Refused because the slot still held a newer or unsettled number. */
    unsigned window_refused;
};

void signal_ledger_init(struct signal_ledger *ledger);

struct signal_ledger_resume {
    /* 1 when a slot hand-over was cut short by the death and finished now. */
    int journal_redone;
    /* Slots that hold a number. */
    unsigned slots_used;
};

/*
 * Takes over the ledger of a dead sink. Every total is counted again
 * from the slots and the retired totals, so a death between two writes
 * of one update leaves no total off by one. -1 when ledger is not a
 * whole ledger (magic, version, size, a slot in the wrong place, a bad
 * class or account); the caller then starts a new ledger and says so.
 * On 0, resumes has grown by one.
 */
int signal_ledger_resume(struct signal_ledger *ledger,
                         struct signal_ledger_resume *out);

struct signal_ledger_close_result {
    unsigned lost;
    unsigned dropped;
    /* Items an earlier close or manifest already put on the books. */
    unsigned repeated;
    /* Marked dropped but already arrived, or URGENT: not counted dropped. */
    unsigned drop_refused;
    /* On -1: the number the ledger refused. */
    uint64_t refused;
};

/*
 * Puts a close or a manifest on the books: issue each number, record
 * the drops a relay marked, count every number that neither arrived nor
 * was dropped as lost, and call on_lost (may be NULL) for each. An item
 * an earlier list already issued is skipped and keeps its account: a
 * source that died before the answer, or the source restarted after it,
 * sends that close again (D-063). 0, or -1 with out->refused set when a
 * number did not fit the ledger; nothing after it is applied.
 */
int signal_ledger_close(struct signal_ledger *ledger,
                        const struct signal_manifest_item *items, unsigned n,
                        void (*on_lost)(uint64_t number, uint32_t class_id,
                                        void *arg),
                        void *arg, struct signal_ledger_close_result *out);

/* 1 if number is in the ring and an issued list or close already had it. */
int signal_ledger_is_issued(const struct signal_ledger *ledger, uint64_t number);

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
