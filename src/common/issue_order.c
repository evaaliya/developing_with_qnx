#include "issue_order.h"

#include <stddef.h>

void issue_order_triple(uint32_t seed, uint32_t triple_index, uint32_t out[3])
{
    uint32_t item[ISSUE_ORDER_TRIPLE];
    uint32_t state;
    int i;

    if (out == NULL) {
        return;
    }
    item[0] = SIGNAL_CLASS_BACKGROUND;
    item[1] = SIGNAL_CLASS_COMMAND;
    item[2] = SIGNAL_CLASS_URGENT;
    state = seed + triple_index * 0x9E3779B9u;
    for (i = ISSUE_ORDER_TRIPLE - 1; i >= 1; i--) {
        uint32_t tmp;
        uint32_t pick;

        state = state * 1664525u + 1013904223u;
        pick = state % (uint32_t)(i + 1);
        tmp = item[i];
        item[i] = item[pick];
        item[pick] = tmp;
    }
    out[0] = item[0];
    out[1] = item[1];
    out[2] = item[2];
}

int issue_order_burst(int use_seed, uint32_t seed, uint32_t urgent,
                      uint32_t command, uint32_t background, uint32_t *out,
                      uint32_t capacity, uint32_t *written)
{
    uint32_t left_u = urgent;
    uint32_t left_c = command;
    uint32_t left_b = background;
    uint32_t total;
    uint32_t count = 0;
    uint32_t triple = 0;

    if (out == NULL || written == NULL) {
        return -1;
    }
    if (command > UINT32_MAX - background ||
        urgent > UINT32_MAX - background - command) {
        return -1;
    }
    total = urgent + command + background;
    if (total > capacity) {
        return -1;
    }

    while (left_b > 0 || left_c > 0 || left_u > 0) {
        if (use_seed && left_b > 0 && left_c > 0 && left_u > 0) {
            uint32_t order[ISSUE_ORDER_TRIPLE];
            int i;

            issue_order_triple(seed, triple, order);
            triple++;
            for (i = 0; i < ISSUE_ORDER_TRIPLE; i++) {
                out[count] = order[i];
                count++;
                if (order[i] == SIGNAL_CLASS_BACKGROUND) {
                    left_b--;
                } else if (order[i] == SIGNAL_CLASS_COMMAND) {
                    left_c--;
                } else {
                    left_u--;
                }
            }
            continue;
        }
        if (left_b > 0) {
            out[count] = SIGNAL_CLASS_BACKGROUND;
            count++;
            left_b--;
        }
        if (left_c > 0) {
            out[count] = SIGNAL_CLASS_COMMAND;
            count++;
            left_c--;
        }
        if (left_u > 0) {
            out[count] = SIGNAL_CLASS_URGENT;
            count++;
            left_u--;
        }
    }
    *written = count;
    return 0;
}
