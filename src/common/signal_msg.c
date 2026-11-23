#include "signal_msg.h"

#include <stddef.h>
#include <stdint.h>

enum signal_verdict signal_check(const struct signal *sig, int64_t now_ns)
{
    int64_t limit;

    if (sig == NULL || sig->deadline_ns < 0) {
        return SIGNAL_LATE;
    }

    /* The sum does not fit in int64. Every representable now_ns is earlier. */
    if (sig->issued_ns > INT64_MAX - sig->deadline_ns) {
        return SIGNAL_ON_TIME;
    }

    limit = sig->issued_ns + sig->deadline_ns;
    if (now_ns <= limit) {
        return SIGNAL_ON_TIME;
    }
    return SIGNAL_LATE;
}

int64_t signal_class_deadline_ns(uint32_t class_id)
{
    switch (class_id) {
    case SIGNAL_CLASS_URGENT:
        return SIGNAL_DEADLINE_URGENT_NS;
    case SIGNAL_CLASS_COMMAND:
        return SIGNAL_DEADLINE_COMMAND_NS;
    case SIGNAL_CLASS_BACKGROUND:
        return SIGNAL_DEADLINE_BACKGROUND_NS;
    default:
        return -1;
    }
}

const char *signal_class_name(uint32_t class_id)
{
    switch (class_id) {
    case SIGNAL_CLASS_URGENT:
        return "URGENT";
    case SIGNAL_CLASS_COMMAND:
        return "COMMAND";
    case SIGNAL_CLASS_BACKGROUND:
        return "BACKGROUND";
    default:
        return "unknown";
    }
}

const char *signal_verdict_name(enum signal_verdict verdict)
{
    if (verdict == SIGNAL_ON_TIME) {
        return "on time";
    }
    return "late";
}
