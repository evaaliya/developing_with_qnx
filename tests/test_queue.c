#include "signal_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect_int(const char *name, int got, int want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %d, want %d)\n", name, got, want);
    failures++;
}

static void expect_u64(const char *name, uint64_t got, uint64_t want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %llu, want %llu)\n", name,
           (unsigned long long)got, (unsigned long long)want);
    failures++;
}

static struct signal make_signal(uint64_t number, uint32_t class_id)
{
    struct signal sig;

    memset(&sig, 0, sizeof sig);
    sig.number = number;
    sig.class_id = class_id;
    sig.issued_ns = 1000;
    sig.deadline_ns = 1000000000LL;
    return sig;
}

static int push(struct signal_queue *q, uint64_t number, uint32_t class_id,
                int64_t arrival_ns, uint64_t arrival_order)
{
    struct signal sig = make_signal(number, class_id);
    return signal_queue_push(q, &sig, arrival_ns, arrival_order);
}

static int contains_number(const struct signal_queue *q, uint64_t number)
{
    size_t i;

    for (i = 0; i < signal_queue_count(q); i++) {
        const struct signal_queue_item *stored = signal_queue_item(q, i);

        if (stored != NULL && stored->body.number == number) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    struct signal_queue q;
    struct signal_queue_item item;
    struct signal sig;
    size_t i;
    int rc;

    memset(&sig, 0, sizeof sig);
    signal_queue_init(&q);
    memset(&item, 0, sizeof item);
    item.body.number = 99;
    expect_int("empty pop", signal_queue_pop(&q, &item), -1);
    expect_u64("empty pop leaves the output alone", item.body.number, 99);
    expect_int("null push", signal_queue_push(NULL, &sig, 0, 0), -1);
    expect_int("null pop", signal_queue_pop(&q, NULL), -1);

    signal_queue_init(&q);
    expect_int("push background", push(&q, 1, SIGNAL_CLASS_BACKGROUND, 10, 1), 0);
    expect_int("push command", push(&q, 2, SIGNAL_CLASS_COMMAND, 20, 2), 0);
    expect_int("push urgent last", push(&q, 3, SIGNAL_CLASS_URGENT, 30, 3), 0);
    expect_int("pop urgent first", signal_queue_pop(&q, &item), 0);
    expect_u64("most urgent is signal 3", item.body.number, 3);
    expect_int("then command", signal_queue_pop(&q, &item), 0);
    expect_u64("command is signal 2", item.body.number, 2);
    expect_int("then background", signal_queue_pop(&q, &item), 0);
    expect_u64("background is signal 1", item.body.number, 1);
    expect_int("empty again", signal_queue_pop(&q, &item), -1);

    signal_queue_init(&q);
    expect_int("same class order 3", push(&q, 30, SIGNAL_CLASS_COMMAND, 5, 3), 0);
    expect_int("same class order 1", push(&q, 10, SIGNAL_CLASS_COMMAND, 5, 1), 0);
    expect_int("same class order 2", push(&q, 20, SIGNAL_CLASS_COMMAND, 5, 2), 0);
    expect_int("tie pop 1", signal_queue_pop(&q, &item), 0);
    expect_u64("equal time, first arrival order", item.body.number, 10);
    expect_int("tie pop 2", signal_queue_pop(&q, &item), 0);
    expect_u64("equal time, second arrival order", item.body.number, 20);
    expect_int("tie pop 3", signal_queue_pop(&q, &item), 0);
    expect_u64("equal time, third arrival order", item.body.number, 30);

    signal_queue_init(&q);
    expect_int("earlier stamp pushed second",
               push(&q, 2, SIGNAL_CLASS_BACKGROUND, 50, 2), 0);
    expect_int("later stamp pushed first",
               push(&q, 1, SIGNAL_CLASS_BACKGROUND, 10, 1), 0);
    expect_int("earlier stamp wins", signal_queue_pop(&q, &item), 0);
    expect_u64("smaller arrival time leaves first", item.body.number, 1);

    signal_queue_init(&q);
    for (i = 0; i < SIGNAL_QUEUE_CAP; i++) {
        rc = push(&q, (uint64_t)(i + 1), SIGNAL_CLASS_COMMAND,
                  (int64_t)(i + 1), (uint64_t)(i + 1));
        if (rc != 0) {
            expect_int("fill to capacity", rc, 0);
            break;
        }
    }
    expect_int("queue is full", (int)signal_queue_count(&q), SIGNAL_QUEUE_CAP);
    sig = make_signal(1000, SIGNAL_CLASS_URGENT);
    expect_int("overflow rejects the new signal",
               signal_queue_push(&q, &sig, 0, 0), -1);
    expect_int("overflow leaves the count unchanged",
               (int)signal_queue_count(&q), SIGNAL_QUEUE_CAP);
    expect_int("overflow pop", signal_queue_pop(&q, &item), 0);
    expect_u64("overflow keeps the original first signal", item.body.number, 1);
    rc = 0;
    for (;;) {
        if (signal_queue_pop(&q, &item) != 0) {
            break;
        }
        if (item.body.number == 1000) {
            rc = 1;
        }
    }
    expect_int("rejected signal is not stored", rc, 0);

    signal_queue_init(&q);
    expect_int("evict setup later background",
               push(&q, 1, SIGNAL_CLASS_BACKGROUND, 100, 1), 0);
    expect_int("evict setup earlier background",
               push(&q, 2, SIGNAL_CLASS_BACKGROUND, 50, 2), 0);
    expect_int("evict setup urgent",
               push(&q, 3, SIGNAL_CLASS_URGENT, 200, 3), 0);
    expect_int("evict setup command",
               push(&q, 4, SIGNAL_CLASS_COMMAND, 300, 4), 0);
    memset(&item, 0, sizeof item);
    expect_int("evict the newest background",
               signal_queue_evict_background(&q, &item), 0);
    expect_u64("newest background is signal 1", item.body.number, 1);
    expect_int("one background remains with both protected classes",
               (int)signal_queue_count(&q), 3);
    expect_int("earlier background stayed", contains_number(&q, 2), 1);
    expect_int("urgent stayed after an eviction", contains_number(&q, 3), 1);
    expect_int("command stayed after an eviction", contains_number(&q, 4), 1);
    expect_int("evicted background is gone", contains_number(&q, 1), 0);
    expect_int("evict the older background",
               signal_queue_evict_background(&q, &item), 0);
    expect_u64("older background is signal 2", item.body.number, 2);
    item.body.number = 77;
    expect_int("evict with no background refuses",
               signal_queue_evict_background(&q, &item), -1);
    expect_u64("a refusal leaves the output alone", item.body.number, 77);
    expect_int("a refusal leaves the count", (int)signal_queue_count(&q), 2);
    expect_int("urgent survived a refused eviction", contains_number(&q, 3), 1);
    expect_int("command survived a refused eviction", contains_number(&q, 4), 1);
    expect_int("pop still takes urgent first", signal_queue_pop(&q, &item), 0);
    expect_u64("urgent was never evicted", item.body.number, 3);
    expect_int("pop then takes command", signal_queue_pop(&q, &item), 0);
    expect_u64("command was never evicted", item.body.number, 4);

    signal_queue_init(&q);
    expect_int("tie background order 1",
               push(&q, 10, SIGNAL_CLASS_BACKGROUND, 5, 1), 0);
    expect_int("tie background order 3",
               push(&q, 30, SIGNAL_CLASS_BACKGROUND, 5, 3), 0);
    expect_int("tie background order 2",
               push(&q, 20, SIGNAL_CLASS_BACKGROUND, 5, 2), 0);
    expect_int("tie urgent is later than every background",
               push(&q, 40, SIGNAL_CLASS_URGENT, 9, 4), 0);
    expect_int("equal time evicts the largest arrival order",
               signal_queue_evict_background(&q, &item), 0);
    expect_u64("largest arrival order is signal 30", item.body.number, 30);
    expect_int("the urgent is still stored", contains_number(&q, 40), 1);
    expect_int("smaller arrival orders stayed", contains_number(&q, 10), 1);

    signal_queue_init(&q);
    expect_int("protected urgent", push(&q, 8, SIGNAL_CLASS_URGENT, 1, 1), 0);
    expect_int("protected command", push(&q, 9, SIGNAL_CLASS_COMMAND, 2, 2), 0);
    item.body.number = 55;
    expect_int("urgent and command alone are not evicted",
               signal_queue_evict_background(&q, &item), -1);
    expect_int("protected queue keeps its count", (int)signal_queue_count(&q), 2);
    expect_u64("protected refusal leaves the output alone", item.body.number, 55);
    expect_int("protected urgent stayed", contains_number(&q, 8), 1);
    expect_int("protected command stayed", contains_number(&q, 9), 1);

    signal_queue_init(&q);
    item.body.number = 66;
    expect_int("evict on an empty queue",
               signal_queue_evict_background(&q, &item), -1);
    expect_u64("empty eviction leaves the output alone", item.body.number, 66);
    expect_int("null evict queue", signal_queue_evict_background(NULL, &item), -1);
    expect_int("null evict output", signal_queue_evict_background(&q, NULL), -1);

    signal_queue_init(&q);
    expect_int("command evict later",
               push(&q, 1, SIGNAL_CLASS_COMMAND, 100, 1), 0);
    expect_int("command evict earlier",
               push(&q, 2, SIGNAL_CLASS_COMMAND, 40, 2), 0);
    expect_int("command evict urgent stays put",
               push(&q, 3, SIGNAL_CLASS_URGENT, 10, 3), 0);
    expect_int("command evict background stays put",
               push(&q, 4, SIGNAL_CLASS_BACKGROUND, 500, 4), 0);
    memset(&item, 0, sizeof item);
    expect_int("evict the newest command",
               signal_queue_evict_newest(&q, SIGNAL_CLASS_COMMAND, &item), 0);
    expect_u64("newest command is signal 1", item.body.number, 1);
    expect_int("three remain after a command eviction",
               (int)signal_queue_count(&q), 3);
    expect_int("earlier command stayed", contains_number(&q, 2), 1);
    expect_int("urgent stayed after a command eviction", contains_number(&q, 3), 1);
    expect_int("background stayed after a command eviction",
               contains_number(&q, 4), 1);
    expect_int("evicted command is gone", contains_number(&q, 1), 0);

    signal_queue_init(&q);
    expect_int("urgent cannot be evicted, setup",
               push(&q, 11, SIGNAL_CLASS_URGENT, 10, 1), 0);
    expect_int("urgent cannot be evicted, later urgent",
               push(&q, 12, SIGNAL_CLASS_URGENT, 90, 2), 0);
    expect_int("urgent cannot be evicted, command",
               push(&q, 13, SIGNAL_CLASS_COMMAND, 20, 3), 0);
    item.body.number = 44;
    expect_int("an urgent cannot be evicted",
               signal_queue_evict_newest(&q, SIGNAL_CLASS_URGENT, &item), -1);
    expect_u64("refusing to evict urgent leaves the output alone",
               item.body.number, 44);
    expect_int("refusing to evict urgent leaves the count",
               (int)signal_queue_count(&q), 3);
    expect_int("first urgent stayed", contains_number(&q, 11), 1);
    expect_int("later urgent stayed", contains_number(&q, 12), 1);
    expect_int("command stayed beside the urgents", contains_number(&q, 13), 1);

    signal_queue_init(&q);
    expect_int("only urgent 1", push(&q, 21, SIGNAL_CLASS_URGENT, 1, 1), 0);
    expect_int("only urgent 2", push(&q, 22, SIGNAL_CLASS_URGENT, 2, 2), 0);
    expect_int("only urgent 3", push(&q, 23, SIGNAL_CLASS_URGENT, 3, 3), 0);
    item.body.number = 33;
    expect_int("a queue of only urgents cannot evict urgent",
               signal_queue_evict_newest(&q, SIGNAL_CLASS_URGENT, &item), -1);
    expect_int("a queue of only urgents has no command to evict",
               signal_queue_evict_newest(&q, SIGNAL_CLASS_COMMAND, &item), -1);
    expect_u64("an all-urgent refusal leaves the output alone",
               item.body.number, 33);
    expect_int("an all-urgent queue keeps its count",
               (int)signal_queue_count(&q), 3);
    expect_int("all-urgent signal 21 stayed", contains_number(&q, 21), 1);
    expect_int("all-urgent signal 22 stayed", contains_number(&q, 22), 1);
    expect_int("all-urgent signal 23 stayed", contains_number(&q, 23), 1);

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
