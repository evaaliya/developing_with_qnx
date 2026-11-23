#ifndef TELEMETRY_PUB_H
#define TELEMETRY_PUB_H

/*
 * Non-blocking delivery toward telemetry (D-054). The signal thread
 * never calls send: it copies a report if the slot is free. A missed
 * copy is replaced by the next report, which carries totals.
 */

#include <stddef.h>

/* 1 when SIGNAL_TELEMETRY is set and non-empty. */
int telem_pub_enabled(void);

/*
 * 1 when the publisher thread is running. 0 when telemetry is off,
 * or the thread could not be started. who is the process name in
 * the error line. Starting failure does not stop the caller.
 */
int telem_pub_open(const char *who);

/* Never waits. Drops the update if the publisher holds the slot. */
void telem_pub_post(const char *text, size_t n);

/*
 * Waits for the slot, then for the publisher thread to send this text.
 * For process exit, not for the signal path.
 */
void telem_pub_flush(const char *text, size_t n);

/* One non-blocking datagram from the calling thread. Ignores errors. */
void telem_send_nb(const char *text, size_t n);

#endif
