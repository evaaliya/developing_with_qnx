#define _QNX_SOURCE

#include "telemetry_pub.h"

#include "signal_sched.h"
#include "telemetry_snap.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/neutrino.h>
#include <sys/socket.h>

/* Below SIGNAL_PRIO_OWN so a publisher never preempts a network node. */
#define TELEM_PRIO 8

_Static_assert(TELEM_PRIO < SIGNAL_PRIO_OWN,
               "telemetry publisher must stay below the network nodes");

static pthread_mutex_t slot_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t pub_thread;
static int pub_started;
static int pub_stop;
static char slot[TELEM_REPORT_CAP];
static size_t slot_len;
static int pub_fd = -1;

static int open_fd(void)
{
    int fd;
    int flags;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_fd(int fd, const char *text, size_t n)
{
    struct sockaddr_in dest;

    if (fd < 0 || text == NULL || n == 0) {
        return;
    }
    memset(&dest, 0, sizeof dest);
    dest.sin_family = AF_INET;
    dest.sin_port = htons(TELEM_UDP_PORT);
    dest.sin_addr.s_addr = htonl(0x7f000001);
    (void)sendto(fd, text, n, MSG_DONTWAIT, (struct sockaddr *)&dest,
                 sizeof dest);
}

void telem_send_nb(const char *text, size_t n)
{
    static int fd = -1;

    if (text == NULL || n == 0) {
        return;
    }
    if (fd < 0) {
        fd = open_fd();
        if (fd < 0) {
            return;
        }
    }
    send_fd(fd, text, n);
}

static void *pub_main(void *arg)
{
    (void)arg;
    /*
     * CPU 1, not CPU 0. The signal chain is pinned to CPU 0. This
     * thread only runs there if the pin fails, and then at priority 8.
     */
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)2) == -1) {
        fprintf(stderr, "telemetry publisher: pinning to CPU 1 failed: %s\n",
                strerror(errno));
    }
    pub_fd = open_fd();
    if (pub_fd < 0) {
        fprintf(stderr, "telemetry publisher: opening udp failed: %s\n",
                strerror(errno));
    }
    for (;;) {
        char local[TELEM_REPORT_CAP];
        size_t n;
        int stopping;
        struct timespec pause;

        pthread_mutex_lock(&slot_mu);
        n = slot_len;
        if (n > sizeof local) {
            n = sizeof local;
        }
        memcpy(local, slot, n);
        stopping = pub_stop;
        pthread_mutex_unlock(&slot_mu);
        if (n > 0) {
            send_fd(pub_fd, local, n);
        }
        if (stopping) {
            break;
        }
        pause.tv_sec = 0;
        pause.tv_nsec = 50000000L;
        nanosleep(&pause, NULL);
    }
    if (pub_fd >= 0) {
        close(pub_fd);
        pub_fd = -1;
    }
    return NULL;
}

int telem_pub_enabled(void)
{
    const char *value = getenv("SIGNAL_TELEMETRY");

    return value != NULL && value[0] != '\0';
}

int telem_pub_open(const char *who)
{
    pthread_attr_t attr;
    struct sched_param param;
    int rc;

    if (!telem_pub_enabled()) {
        return 0;
    }
    if (pub_started) {
        return 1;
    }
    if (who == NULL) {
        who = "node";
    }
    memset(&param, 0, sizeof param);
    param.sched_priority = TELEM_PRIO;
    if (pthread_attr_init(&attr) != 0) {
        fprintf(stderr, "%s: telemetry publisher attribute failed\n", who);
        return 0;
    }
    if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0 ||
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0 ||
        pthread_attr_setschedparam(&attr, &param) != 0) {
        fprintf(stderr, "%s: telemetry publisher priority failed\n", who);
        pthread_attr_destroy(&attr);
        return 0;
    }
    rc = pthread_create(&pub_thread, &attr, pub_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "%s: telemetry publisher did not start: %s\n", who,
                strerror(rc));
        return 0;
    }
    pub_started = 1;
    return 1;
}

void telem_pub_post(const char *text, size_t n)
{
    if (!pub_started || text == NULL || n == 0 || n >= sizeof slot) {
        return;
    }
    if (pthread_mutex_trylock(&slot_mu) != 0) {
        return;
    }
    if (pub_stop) {
        pthread_mutex_unlock(&slot_mu);
        return;
    }
    memcpy(slot, text, n);
    slot[n] = '\0';
    slot_len = n;
    pthread_mutex_unlock(&slot_mu);
}

void telem_pub_flush(const char *text, size_t n)
{
    if (!pub_started || text == NULL || n == 0 || n >= sizeof slot) {
        return;
    }
    pthread_mutex_lock(&slot_mu);
    memcpy(slot, text, n);
    slot[n] = '\0';
    slot_len = n;
    pub_stop = 1;
    pthread_mutex_unlock(&slot_mu);
    pthread_join(pub_thread, NULL);
    pub_started = 0;
}
