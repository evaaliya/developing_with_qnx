#ifndef ISSUE_ORDER_H
#define ISSUE_ORDER_H

/*
 * Order of classes inside one source burst. No clock and no rand:
 * the same seed and triple index always produce the same triple.
 * A full triple is one BACKGROUND, one COMMAND, and one URGENT.
 */

#include "signal_msg.h"

#include <stdint.h>

#define ISSUE_ORDER_TRIPLE 3

/* out must hold three classes. A NULL out is ignored. */
void issue_order_triple(uint32_t seed, uint32_t triple_index, uint32_t out[3]);

/*
 * Writes the class of each signal. use_seed == 0 keeps the fixed
 * order BACKGROUND, then COMMAND, then URGENT, while any remain.
 * use_seed != 0 shuffles each triple that still has one of every
 * class. Leftovers stay in that fixed order.
 * 0 and *written is the number of classes. -1 on a bad argument.
 * On -1, *written and the buffer are left unchanged.
 */
int issue_order_burst(int use_seed, uint32_t seed, uint32_t urgent,
                      uint32_t command, uint32_t background, uint32_t *out,
                      uint32_t capacity, uint32_t *written);

#endif
