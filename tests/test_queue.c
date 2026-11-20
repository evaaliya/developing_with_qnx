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

    if (failures != 0) {
        printf("%d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
