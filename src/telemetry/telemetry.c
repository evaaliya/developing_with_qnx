#define _QNX_SOURCE

#include "telemetry_snap.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
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

/* Same priority as the publisher thread: below every network node. */
#define TELEM_PRIO 8

static int64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "telemetry: reading the monotonic clock failed: %s\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

static void sit_aside(void)
{
    struct sched_param param;

    memset(&param, 0, sizeof param);
    param.sched_priority = TELEM_PRIO;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        fprintf(stderr, "telemetry: lowering priority failed: %s\n",
                strerror(errno));
    }
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)2) == -1) {
        fprintf(stderr, "telemetry: pinning to CPU 1 failed: %s\n",
                strerror(errno));
    }
}

static int listen_tcp(void)
{
    struct sockaddr_in addr;
    struct timespec pause;
    int fd;
    int on = 1;
    int attempt;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "telemetry: opening tcp failed: %s\n", strerror(errno));
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == -1 ||
        set_nonblock(fd) != 0) {
        fprintf(stderr, "telemetry: preparing tcp failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TELEM_TCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    /*
     * qconn in this image already listens on TCP 8000 (F-025). The
     * scripts stop it. A restart can also arrive before the previous
     * socket is gone, so EADDRINUSE is retried for a short time.
     */
    for (attempt = 0; attempt < 40; attempt++) {
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            break;
        }
        if (errno != EADDRINUSE || attempt == 39) {
            fprintf(stderr, "telemetry: listening on tcp %d failed: %s\n",
                    TELEM_TCP_PORT, strerror(errno));
            close(fd);
            return -1;
        }
        pause.tv_sec = 0;
        pause.tv_nsec = 50000000L;
        nanosleep(&pause, NULL);
    }
    if (listen(fd, 4) == -1) {
        fprintf(stderr, "telemetry: listening on tcp %d failed: %s\n",
                TELEM_TCP_PORT, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int bind_udp(void)
{
    struct sockaddr_in addr;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "telemetry: opening udp failed: %s\n", strerror(errno));
        return -1;
    }
    if (set_nonblock(fd) != 0) {
        fprintf(stderr, "telemetry: preparing udp failed: %s\n",
                strerror(errno));
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TELEM_UDP_PORT);
    addr.sin_addr.s_addr = htonl(0x7f000001);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
        fprintf(stderr, "telemetry: binding udp %d failed: %s\n",
                TELEM_UDP_PORT, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void drop_client(int *client)
{
    if (*client >= 0) {
        close(*client);
        *client = -1;
        printf("telemetry: client closed\n");
        fflush(stdout);
    }
}

static int write_snapshot(int client, struct telem_snapshot *snap, int64_t now,
                          int with_arm)
{
    char buf[TELEM_REPORT_CAP];
    int n;
    size_t off = 0;

    snap->time_ns = now;
    snap->seq++;
    /* Only a client that sent "arm" gets the arm line (D-080). */
    n = with_arm ? telem_snapshot_format_arm(snap, buf, sizeof buf)
                 : telem_snapshot_format(snap, buf, sizeof buf);
    if (n < 0) {
        snap->seq--;
        fprintf(stderr, "telemetry: the snapshot did not fit\n");
        return -1;
    }
    while (off < (size_t)n) {
        ssize_t wrote = write(client, buf + off, (size_t)n - off);

        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            snap->seq--;
            return -1;
        }
        if (wrote == 0) {
            snap->seq--;
            return -1;
        }
        off += (size_t)wrote;
    }
    return 0;
}

static void drain_udp(int udp, struct telem_snapshot *snap, uint64_t *reports)
{
    for (;;) {
        char buf[TELEM_REPORT_CAP];
        ssize_t n = recvfrom(udp, buf, sizeof buf - 1, 0, NULL, NULL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            fprintf(stderr, "telemetry: reading a report failed: %s\n",
                    strerror(errno));
            return;
        }
        buf[n] = '\0';
        if (telem_snapshot_merge_report(snap, buf) != 0) {
            fprintf(stderr,
                    "telemetry: ignored a report that is not a snapshot "
                    "fragment\n");
            continue;
        }
        (*reports)++;
    }
}

int main(void)
{
    struct telem_snapshot snap;
    int tcp;
    int udp;
    int client = -1;
    int client_arm = 0;
    uint64_t reports = 0;
    uint64_t emitted_reports = 0;
    int64_t last_emit_ns = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        fprintf(stderr, "telemetry: ignoring SIGPIPE failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    sit_aside();
    telem_snapshot_init(&snap);
    tcp = listen_tcp();
    udp = bind_udp();
    if (tcp < 0 || udp < 0) {
        return EXIT_FAILURE;
    }
    printf("telemetry: listening on tcp %d and udp %d\n", TELEM_TCP_PORT,
           TELEM_UDP_PORT);
    fflush(stdout);

    for (;;) {
        struct pollfd fds[3];
        int nfd = 0;
        int udp_slot;
        int tcp_slot;
        int client_slot = -1;
        int ready;
        int64_t now;

        fds[nfd].fd = udp;
        fds[nfd].events = POLLIN;
        udp_slot = nfd++;
        fds[nfd].fd = tcp;
        fds[nfd].events = POLLIN;
        tcp_slot = nfd++;
        if (client >= 0) {
            fds[nfd].fd = client;
            fds[nfd].events = POLLIN;
            client_slot = nfd++;
        }
        ready = poll(fds, (nfds_t)nfd, 50);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "telemetry: waiting for a client failed: %s\n",
                    strerror(errno));
            return EXIT_FAILURE;
        }
        if (ready > 0 && (fds[udp_slot].revents & POLLIN) != 0) {
            drain_udp(udp, &snap, &reports);
        }
        if (ready > 0 && (fds[tcp_slot].revents & POLLIN) != 0) {
            int fd = accept(tcp, NULL, NULL);

            if (fd >= 0) {
                if (set_nonblock(fd) != 0) {
                    close(fd);
                } else {
                    if (client >= 0) {
                        close(client);
                        printf("telemetry: replaced the client\n");
                        fflush(stdout);
                    } else {
                        printf("telemetry: client connected\n");
                        fflush(stdout);
                    }
                    client = fd;
                    client_arm = 0;
                    last_emit_ns = 0;
                }
            }
        }
        if (client_slot >= 0 &&
            (fds[client_slot].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char junk[256];
            ssize_t got = read(client, junk, sizeof junk - 1);

            if (got > 0) {
                junk[got] = '\0';
                if (strstr(junk, "arm") != NULL && !client_arm) {
                    client_arm = 1;
                    printf("telemetry: the client asked for the arm\n");
                    fflush(stdout);
                }
            }
            if (got == 0 ||
                (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR)) {
                drop_client(&client);
            }
        }
        if (client < 0) {
            continue;
        }
        now = monotonic_ns();
        if (last_emit_ns != 0 && now - last_emit_ns < 50000000LL) {
            continue;
        }
        if (last_emit_ns != 0 && reports == emitted_reports &&
            now - last_emit_ns < 200000000LL) {
            continue;
        }
        if (write_snapshot(client, &snap, now, client_arm) != 0) {
            drop_client(&client);
            continue;
        }
        last_emit_ns = now;
        emitted_reports = reports;
    }
}
