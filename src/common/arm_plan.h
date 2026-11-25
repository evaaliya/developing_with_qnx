#ifndef ARM_PLAN_H
#define ARM_PLAN_H

/*
 * The planned stop at the eye (D-089, D-090). The stop margin is the
 * URGENT deadline times the approach speed: a chain that keeps its
 * deadline cannot reach the eye. "person in" comes when the tool is the
 * margin plus the braking path from the eye, so with no delay the arm
 * rests at the planned point, and every millisecond of delay is 0.25 mm
 * past it.
 *
 * The sink publishes when the approach began in a small shared object;
 * the source (the zone sensor that sees the arm) reads it there, not
 * over the network, and issues the event at the computed instant.
 * POSIX only: the host tests use the same code.
 */

#include "arm_model.h"
#include "signal_msg.h"

#include <stdint.h>

/* 31 ms x 250 mm/s = 7.75 mm. */
#define ARM_PLAN_MARGIN_M (ARM_SPEED_M_S * (double)SIGNAL_DEADLINE_URGENT_NS * 1e-9)

/* Margin plus the braking path from the approach speed: 32.75 mm. */
double arm_plan_event_distance_m(void);

/*
 * Time from the start of an approach from the home pose toward the eye
 * pose until the tool is dist_m from the eye, by the same model the sink
 * runs (bisection to 1 ns). -1 when the tool never comes that close.
 */
int64_t arm_plan_time_to_distance_ns(double dist_m);

/* The shared object the sink writes and the source reads (D-090). */
#define ARM_APPROACH_STORE "/signal-sink-arm"
#define ARM_APPROACH_MAGIC 0x41524d31u

struct arm_approach {
    uint32_t magic;
    /* Eye distance of the tool when the approach began, micrometres. */
    int32_t start_eye_um;
    /* CLOCK_MONOTONIC ns of the start; 0 until an approach begins. */
    int64_t start_ns;
};

/* The writer: the eye distance first, then the start time (release). */
void arm_approach_publish(struct arm_approach *a, int64_t start_ns,
                          int32_t start_eye_um);

/*
 * The reader: 1 with the start and eye distance once an approach has
 * begun (acquire), 0 when none has, -1 when the object is not ours.
 */
int arm_approach_read(const struct arm_approach *a, int64_t *start_ns,
                      int32_t *start_eye_um);

/*
 * Maps an existing object read-only. NULL with errno set when there is
 * none (the sink has not started) or it is too short.
 */
const struct arm_approach *arm_approach_open(const char *name);

void arm_approach_close(const struct arm_approach *a);

#endif
