#define _QNX_SOURCE

#include "issued_log.h"
#include "node_fault.h"
#include "restart_limit.h"
#include "telemetry_pub.h"
#include "telemetry_snap.h"
#include "wire.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/neutrino.h>

extern char **environ;

#define FIND_ATTEMPTS 50
#define FIND_PAUSE_NS 50000000L
#define PROGRESS_NS 5000000000LL
#define TICK_NS 200000000LL

enum role {
    ROLE_SINK = 0,
    ROLE_RELAY_B,
    ROLE_RELAY_A,
    ROLE_SOURCE,
    ROLE_COUNT
};

struct watched {
    const char *role;
    const char *log_path;
    const char *name;
    pid_t pid;
    int coid;
    struct restart_limit limit;
    int64_t progress_due_ns;
    int starts;
    int exited_ok;
};

static struct watched nodes[ROLE_COUNT];
static int shutting_down;
static int locked;
static unsigned long urgent_count = 3;
static unsigned long command_count = 3;
static unsigned long background_count = 3;
static unsigned long gap_ms;
static unsigned long window_ms = 1000;
static uint64_t source_next = 1;
static int source_restart;
static int live_source;
static pid_t telem_pid;
static int telem_starts;
static int telem_restart_due;
static int64_t telem_restart_at;

static int64_t monotonic_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "supervisor: reading the monotonic clock failed: %s\n",
                strerror(errno));
        exit(EXIT_FAILURE);
    }
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static int parse_u(const char *text, unsigned long *out)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0) {
        return -1;
    }
    *out = value;
    return 0;
}

static int open_name(const char *name)
{
    int attempt;

    for (attempt = 0; attempt < FIND_ATTEMPTS; attempt++) {
        int coid = name_open(name, 0);
        struct timespec pause;

        if (coid != -1) {
            return coid;
        }
        if (errno != ENOENT) {
            fprintf(stderr, "supervisor: opening %s failed: %s\n",
                    name, strerror(errno));
            return -1;
        }
        pause.tv_sec = 0;
        pause.tv_nsec = FIND_PAUSE_NS;
        nanosleep(&pause, NULL);
    }
    fprintf(stderr, "supervisor: could not find a node registered as %s\n", name);
    return -1;
}

static pid_t spawn_logged(const char *path, char *const argv[], const char *log,
                          int truncate)
{
    posix_spawn_file_actions_t actions;
    pid_t pid;
    int rc;
    int flags = O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND);

    rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        fprintf(stderr, "supervisor: file actions failed: %s\n", strerror(rc));
        return -1;
    }
    rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log, flags, 0644);
    if (rc != 0) {
        fprintf(stderr, "supervisor: opening %s for the child failed: %s\n",
                log, strerror(rc));
        posix_spawn_file_actions_destroy(&actions);
        return -1;
    }
    rc = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (rc != 0) {
        fprintf(stderr, "supervisor: dup of stderr failed: %s\n", strerror(rc));
        posix_spawn_file_actions_destroy(&actions);
        return -1;
    }
    rc = posix_spawn(&pid, path, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        fprintf(stderr, "supervisor: starting %s failed: %s\n", path, strerror(rc));
        return -1;
    }
    return pid;
}

static int start_node(enum role role, int restarted)
{
    struct watched *node = &nodes[role];
    char count_buf[32];
    char urgent_buf[32];
    char command_buf[32];
    char background_buf[32];
    char gap_buf[32];
    char window_buf[32];
    char next_buf[32];
    char *argv[12];
    char *path;
    pid_t pid;
    int truncate = node->starts == 0;

    switch (role) {
    case ROLE_SINK:
        if (live_source) {
            snprintf(count_buf, sizeof count_buf, "%d", SIGNAL_MANIFEST_CAP);
        } else {
            snprintf(count_buf, sizeof count_buf, "%lu",
                     urgent_count + command_count + background_count);
        }
        snprintf(window_buf, sizeof window_buf, "%lu", window_ms);
        path = "/tmp/sink";
        argv[0] = path;
        /* -r: go on with the dead node's books (D-063). */
        argv[1] = restarted ? "-r" : count_buf;
        argv[2] = restarted ? count_buf : window_buf;
        argv[3] = restarted ? window_buf : NULL;
        argv[4] = NULL;
        break;
    case ROLE_RELAY_B:
        path = "/tmp/relay";
        argv[0] = path;
        argv[1] = "signal-relay-b";
        argv[2] = "signal-sink";
        argv[3] = restarted ? "-r" : NULL;
        argv[4] = NULL;
        break;
    case ROLE_RELAY_A:
        path = "/tmp/relay";
        argv[0] = path;
        argv[1] = "signal-relay-a";
        argv[2] = "signal-relay-b";
        argv[3] = restarted ? "-r" : NULL;
        argv[4] = NULL;
        break;
    case ROLE_SOURCE: {
        int n = 0;

        if (live_source) {
            path = "/tmp/source";
            argv[n++] = path;
            argv[n++] = "live";
            if (restarted) {
                argv[n++] = "-r";
            }
            argv[n++] = "signal-relay-a";
            argv[n] = NULL;
            break;
        }
        snprintf(urgent_buf, sizeof urgent_buf, "%lu", urgent_count);
        snprintf(command_buf, sizeof command_buf, "%lu", command_count);
        snprintf(background_buf, sizeof background_buf, "%lu", background_count);
        snprintf(gap_buf, sizeof gap_buf, "%lu", gap_ms);
        path = "/tmp/source";
        argv[n++] = path;
        if (source_restart) {
            snprintf(next_buf, sizeof next_buf, "%llu",
                     (unsigned long long)source_next);
            argv[n++] = "-n";
            argv[n++] = next_buf;
        }
        argv[n++] = urgent_buf;
        argv[n++] = command_buf;
        argv[n++] = background_buf;
        argv[n++] = "signal-relay-a";
        if (gap_ms > 0) {
            argv[n++] = gap_buf;
        }
        argv[n] = NULL;
        break;
    }
    default:
        return -1;
    }

    pid = spawn_logged(path, argv, node->log_path, truncate);
    if (pid < 0) {
        return -1;
    }
    node->pid = pid;
    node->starts++;
    node->exited_ok = 0;
    node->progress_due_ns = 0;
    if (node->coid >= 0) {
        name_close(node->coid);
        node->coid = -1;
    }
    if (node->name != NULL) {
        int coid = open_name(node->name);

        if (coid < 0) {
            return -1;
        }
        node->coid = coid;
    }
    printf("supervisor: %s %s pid=%d\n",
           restarted ? "restarted" : "node", node->role, (int)pid);
    fflush(stdout);
    return 0;
}

static void kill_all(void)
{
    int i;

    for (i = 0; i < ROLE_COUNT; i++) {
        if (nodes[i].pid > 0) {
            kill(nodes[i].pid, SIGKILL);
        }
    }
    if (telem_pid > 0) {
        kill(telem_pid, SIGKILL);
    }
}

static void send_lock(void)
{
    struct signal_lock body;
    uint64_t timeout = 2000000000ULL;
    int coid = nodes[ROLE_SINK].coid;
    long sent;

    if (coid < 0) {
        fprintf(stderr, "supervisor: no connection to sink for LOCKED_SAFE\n");
        return;
    }
    memset(&body, 0, sizeof body);
    body.type = SIGNAL_MSG_LOCK;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     NULL, &timeout, NULL) == -1) {
        fprintf(stderr, "supervisor: arming the lock send failed: %s\n",
                strerror(errno));
        return;
    }
    sent = MsgSend(coid, &body, sizeof body, NULL, 0);
    if (sent == -1) {
        fprintf(stderr, "supervisor: telling sink to lock failed: %s\n",
                strerror(errno));
    }
}

static int read_text(const char *path, char *buf, size_t cap, size_t *used)
{
    FILE *file;
    size_t n;

    file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "supervisor: opening %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    n = fread(buf, 1, cap - 1, file);
    if (ferror(file)) {
        fprintf(stderr, "supervisor: reading %s failed: %s\n", path, strerror(errno));
        fclose(file);
        return -1;
    }
    if (!feof(file)) {
        fprintf(stderr, "supervisor: %s is too large\n", path);
        fclose(file);
        return -1;
    }
    fclose(file);
    buf[n] = '\0';
    *used = n;
    return 0;
}

/* Copy printed source lines so the restarted source can rebuild the list. */
static int snapshot_source(uint64_t *next)
{
    char text[16384];
    static struct issued_item items[SIGNAL_MANIFEST_CAP];
    unsigned count = 0;
    uint64_t max_number = 0;
    uint64_t restart_at = 0;
    FILE *out;
    unsigned i;
    size_t used = 0;

    if (read_text("/tmp/source.out", text, sizeof text, &used) != 0) {
        return -1;
    }
    if (issued_list_parse(text, items, SIGNAL_MANIFEST_CAP, &count,
                          &max_number) != 0 ||
        issued_list_restart_number(max_number, count > 0, &restart_at) != 0) {
        fprintf(stderr, "supervisor: the source log is not a list of sent signals\n");
        return -1;
    }
    out = fopen(ISSUED_LIST_PATH, "w");
    if (out == NULL) {
        fprintf(stderr, "supervisor: opening %s failed: %s\n",
                ISSUED_LIST_PATH, strerror(errno));
        return -1;
    }
    for (i = 0; i < count; i++) {
        char line[128];

        if (issued_list_line(&items[i], line, sizeof line) != 0 ||
            fputs(line, out) == EOF) {
            fprintf(stderr, "supervisor: writing %s failed: %s\n",
                    ISSUED_LIST_PATH, strerror(errno));
            fclose(out);
            return -1;
        }
    }
    if (fclose(out) == EOF) {
        fprintf(stderr, "supervisor: closing %s failed: %s\n",
                ISSUED_LIST_PATH, strerror(errno));
        return -1;
    }
    *next = restart_at;
    printf("supervisor: source issued list %u, next %llu\n",
           count, (unsigned long long)restart_at);
    fflush(stdout);
    return 0;
}

static int handle_status(enum role role, int status, const char *how)
{
    struct watched *node = &nodes[role];
    int signaled = WIFSIGNALED(status);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;

    printf("supervisor: %s %s pid=%d exit=%d signal=%d\n",
           node->role, how, (int)node->pid, code,
           signaled ? WTERMSIG(status) : 0);
    fflush(stdout);

    if (WIFEXITED(status) && code == 0) {
        node->exited_ok = 1;
        node->pid = 0;
        if (node->coid >= 0) {
            name_close(node->coid);
            node->coid = -1;
        }
        return 0;
    }

    if (shutting_down) {
        node->pid = 0;
        return 0;
    }

    node->pid = 0;
    if (node->coid >= 0) {
        name_close(node->coid);
        node->coid = -1;
    }
    /*
     * A stopped child is not a death: waitpid is not passed WUNTRACED,
     * so this function does not run for SIGSTOP (D-052).
     */
    if (node_fault_action(NODE_DEAD, &node->limit) != NODE_ACTION_RESTART) {
        printf("supervisor: LOCKED_SAFE\n");
        fflush(stdout);
        locked = 1;
        shutting_down = 1;
        send_lock();
        kill_all();
        return 0;
    }
    printf("supervisor: %s streak=%d limit=%d\n",
           node->role, node->limit.streak, node->limit.limit);
    fflush(stdout);
    if (role == ROLE_SOURCE && !live_source) {
        if (snapshot_source(&source_next) != 0) {
            fprintf(stderr, "supervisor: could not keep the source issued list\n");
            return -1;
        }
        source_restart = 1;
    }
    {
        struct timespec pause;
        unsigned long hold_ms = window_ms + 500UL;

        /*
         * On this guest a new process is back before the urgent window
         * elapses. Hold one window so the missing pulse is visible.
         */
        printf("supervisor: holding restart of %s for %lu ms\n",
               node->role, hold_ms);
        fflush(stdout);
        pause.tv_sec = (time_t)(hold_ms / 1000UL);
        pause.tv_nsec = (long)(hold_ms % 1000UL) * 1000000L;
        nanosleep(&pause, NULL);
    }
    if (shutting_down) {
        return 0;
    }
    if (start_node(role, 1) != 0) {
        fprintf(stderr, "supervisor: restart of %s failed\n", node->role);
        return -1;
    }
    nodes[role].progress_due_ns = monotonic_ns() + PROGRESS_NS;
    return 0;
}

static void reap(enum role role, int block, const char *how)
{
    int status;
    pid_t got;

    if (nodes[role].pid <= 0) {
        return;
    }
    got = waitpid(nodes[role].pid, &status, block ? 0 : WNOHANG);
    if (got == nodes[role].pid) {
        handle_status(role, status, how);
    }
}

static void note_pulse(int coid)
{
    int i;

    if (ConnectServerInfo(0, coid, NULL) == coid) {
        printf("supervisor: stale death pulse coid=%d\n", coid);
        fflush(stdout);
        return;
    }
    for (i = 0; i < ROLE_COUNT; i++) {
        if (nodes[i].coid != coid) {
            continue;
        }
        printf("supervisor: death pulse %s coid=%d\n", nodes[i].role, coid);
        fflush(stdout);
        nodes[i].coid = -1;
        name_close(coid);
        reap((enum role)i, 1, "pulse");
        return;
    }
}

static void note_progress(int64_t now_ns)
{
    int i;

    for (i = 0; i < ROLE_COUNT; i++) {
        struct watched *node = &nodes[i];

        if (node->progress_due_ns == 0 || node->pid <= 0 ||
            now_ns < node->progress_due_ns) {
            continue;
        }
        if (kill(node->pid, 0) == 0) {
            restart_limit_on_progress(&node->limit);
            printf("supervisor: %s progress, streak cleared\n", node->role);
            fflush(stdout);
        }
        node->progress_due_ns = 0;
    }
}

static int start_telemetry(int restarted)
{
    char *path = "/tmp/telemetry";
    char *argv[] = {"/tmp/telemetry", NULL};
    pid_t pid = spawn_logged(path, argv, "/tmp/telemetry.out", telem_starts == 0);

    if (pid < 0) {
        return -1;
    }
    telem_pid = pid;
    telem_starts++;
    printf("supervisor: %s telemetry pid=%d\n",
           restarted ? "restarted" : "node", (int)pid);
    fflush(stdout);
    return 0;
}

static void publish_nodes(void)
{
    struct telem_node report[TELEM_NODES];
    char buf[TELEM_REPORT_CAP];
    static const enum role order[TELEM_NODES] = {
        ROLE_SOURCE, ROLE_RELAY_A, ROLE_RELAY_B, ROLE_SINK
    };
    int i;
    int n;

    for (i = 0; i < TELEM_NODES; i++) {
        struct watched *node = &nodes[order[i]];

        report[i].alive = node->pid > 0 ? 1 : 0;
        report[i].restarts =
            node->starts > 0 ? (uint32_t)(node->starts - 1) : 0;
    }
    n = telem_report_nodes(buf, sizeof buf, report);
    if (n > 0) {
        telem_send_nb(buf, (size_t)n);
    }
}

static void reap_telemetry(int block)
{
    int status;
    pid_t got;

    if (telem_pid <= 0) {
        return;
    }
    got = waitpid(telem_pid, &status, block ? 0 : WNOHANG);
    if (got != telem_pid) {
        return;
    }
    printf("supervisor: telemetry pid=%d exit=%d signal=%d\n", (int)telem_pid,
           WIFEXITED(status) ? WEXITSTATUS(status) : 0,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    fflush(stdout);
    telem_pid = 0;
    if (shutting_down) {
        return;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return;
    }
    /*
     * Telemetry is not a network node. Restart it without the limit
     * that locks the path, and do not tell sink to enter SAFE.
     * The pause keeps a process that exits at once from being
     * spawned on every receive tick.
     */
    printf("supervisor: telemetry is not a network node, K unchanged\n");
    fflush(stdout);
    telem_restart_due = 1;
    telem_restart_at = monotonic_ns() + 500000000LL;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: supervisor -k K [-u N] [-c N] [-b N] [-g ms] [-w ms] [-L]\n");
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    unsigned long k_value = 0;
    int have_k = 0;
    int chid;
    int i;
    int64_t now_ns;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    for (i = 1; i < argc; i++) {
        unsigned long value;

        if (strcmp(argv[i], "-L") == 0) {
            live_source = 1;
            continue;
        }
        if (i + 1 >= argc || parse_u(argv[i + 1], &value) != 0) {
            return usage();
        }
        if (strcmp(argv[i], "-k") == 0) {
            k_value = value;
            have_k = 1;
        } else if (strcmp(argv[i], "-u") == 0) {
            urgent_count = value;
        } else if (strcmp(argv[i], "-c") == 0) {
            command_count = value;
        } else if (strcmp(argv[i], "-b") == 0) {
            background_count = value;
        } else if (strcmp(argv[i], "-g") == 0) {
            gap_ms = value;
        } else if (strcmp(argv[i], "-w") == 0) {
            window_ms = value;
        } else {
            return usage();
        }
        i++;
    }
    if (!have_k || k_value < 1 || window_ms < 1 ||
        urgent_count + command_count + background_count == 0 ||
        urgent_count + command_count + background_count > SIGNAL_MANIFEST_CAP) {
        return usage();
    }

    nodes[ROLE_SINK] = (struct watched){
        "sink", "/tmp/sink.out", "signal-sink", 0, -1, {0, 0, 0}, 0, 0, 0};
    nodes[ROLE_RELAY_B] = (struct watched){
        "relay-b", "/tmp/relay-b.out", "signal-relay-b", 0, -1, {0, 0, 0}, 0, 0, 0};
    nodes[ROLE_RELAY_A] = (struct watched){
        "relay-a", "/tmp/relay-a.out", "signal-relay-a", 0, -1, {0, 0, 0}, 0, 0, 0};
    nodes[ROLE_SOURCE] = (struct watched){
        "source", "/tmp/source.out", NULL, 0, -1, {0, 0, 0}, 0, 0, 0};
    for (i = 0; i < ROLE_COUNT; i++) {
        restart_limit_init(&nodes[i].limit, (int)k_value);
    }

    if (unlink(ISSUED_LIST_PATH) == -1 && errno != ENOENT) {
        fprintf(stderr, "supervisor: removing %s failed: %s\n",
                ISSUED_LIST_PATH, strerror(errno));
        return EXIT_FAILURE;
    }
    source_restart = 0;
    source_next = 1;

    chid = ChannelCreate(_NTO_CHF_COID_DISCONNECT | _NTO_CHF_PRIVATE);
    if (chid == -1) {
        fprintf(stderr, "supervisor: creating the death channel failed: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }
    printf("supervisor: watching with COIDDEATH, K=%lu\n", k_value);
    fflush(stdout);

    if (setenv("SIGNAL_TELEMETRY", "1", 1) != 0) {
        fprintf(stderr, "supervisor: could not enable telemetry reports: %s\n",
                strerror(errno));
    }
    if (start_telemetry(0) != 0) {
        fprintf(stderr,
                "supervisor: telemetry did not start; the network still starts\n");
    }

    if (start_node(ROLE_SINK, 0) != 0 || start_node(ROLE_RELAY_B, 0) != 0 ||
        start_node(ROLE_RELAY_A, 0) != 0 || start_node(ROLE_SOURCE, 0) != 0) {
        shutting_down = 1;
        kill_all();
        return EXIT_FAILURE;
    }

    for (;;) {
        struct _pulse pulse;
        uint64_t timeout = TICK_NS;
        rcvid_t rcvid;

        now_ns = monotonic_ns();
        note_progress(now_ns);
        publish_nodes();
        reap_telemetry(0);
        if (telem_restart_due && !shutting_down && now_ns >= telem_restart_at) {
            telem_restart_due = 0;
            if (start_telemetry(1) != 0) {
                fprintf(stderr, "supervisor: restart of telemetry failed\n");
                telem_restart_due = 1;
                telem_restart_at = now_ns + 500000000LL;
            }
        }
        if (nodes[ROLE_SOURCE].exited_ok && nodes[ROLE_SINK].exited_ok) {
            shutting_down = 1;
            kill_all();
            break;
        }
        for (i = 0; i < ROLE_COUNT; i++) {
            reap((enum role)i, 0, "wait");
        }
        if (locked) {
            break;
        }
        if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &timeout,
                         NULL) == -1) {
            fprintf(stderr, "supervisor: arming the receive timeout failed: %s\n",
                    strerror(errno));
            shutting_down = 1;
            kill_all();
            return EXIT_FAILURE;
        }
        rcvid = MsgReceive(chid, &pulse, sizeof pulse, NULL);
        if (rcvid == -1) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            fprintf(stderr, "supervisor: waiting for a death pulse failed: %s\n",
                    strerror(errno));
            shutting_down = 1;
            kill_all();
            return EXIT_FAILURE;
        }
        if (rcvid == 0 && pulse.code == _PULSE_CODE_COIDDEATH) {
            note_pulse(pulse.value.sival_int);
        }
    }

    shutting_down = 1;
    for (i = 0; i < ROLE_COUNT; i++) {
        reap((enum role)i, 1, "stop");
    }
    reap_telemetry(1);
    if (locked || nodes[ROLE_SINK].exited_ok) {
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
