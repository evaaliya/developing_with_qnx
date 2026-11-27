/*
 * Start and restart of the "QNX tools" network with HAM (D-098 (6)).
 *
 *   launch start <bindir> <sink-gap-ms>
 *       Starts HAM from <bindir>/ham/ham when /proc/ham is missing, then
 *       for each node in the order sink, relay B, relay A, source:
 *       ham_attach(name, 0, -1, full command line, 0) (HAM starts the
 *       process itself), ham_condition(CONDDEATH, HREARMAFTERRESTART) and
 *       ham_action_restart(the same line, HREARMAFTERRESTART). It waits
 *       for each node's name before the next one, then exits. A restarted
 *       node starts from zero: no books, no number carried over.
 *   launch stop
 *       ham_detach_name for the four entities, so a clean exit after
 *       "stop" is not restarted.
 *   launch kill <log> <count> <pause-ms>
 *       The restart check: SIGKILL the process named by the last
 *       "<node>: pid P start_ns T" line in <log>, wait for a new such
 *       line with another pid, print the time from the kill to the new
 *       start on one CLOCK_MONOTONIC; count times, pause-ms apart.
 *
 * Every node writes its own log with -o (HAM runs a plain command line,
 * no shell); SIGNAL_QUIET=1 in this process adds -q to every node.
 */
#define _QNX_SOURCE

#include <errno.h>
#include <ha/ham.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define WAIT_UP_NS 5000000000LL
#define WAIT_RESTART_NS 10000000000LL

struct node {
    const char *entity;
    /* What appears once the node is up. */
    const char *ready_path;
    const char *log;
};

static const struct node g_nodes[4] = {
    { "qt-sink", "/dev/name/local/signal-sink", "/tmp/sink.out" },
    { "qt-relay-b", "/dev/name/local/signal-relay-b", "/tmp/relay-b.out" },
    { "qt-relay-a", "/dev/name/local/signal-relay-a", "/tmp/relay-a.out" },
    { "qt-source", "/dev/signal-source", "/tmp/source.out" },
};

static int64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void nap_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}

static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int wait_for(const char *path, int64_t limit_ns)
{
    int64_t until = now_ns() + limit_ns;

    while (now_ns() < until) {
        if (exists(path)) {
            return 1;
        }
        nap_ms(5);
    }
    return exists(path);
}

/* HAM itself, from the copy in <bindir>/ham, with its library beside it. */
static int start_ham(const char *bindir)
{
    char ham[256];
    char log[256];
    char libpath[300];
    char *argv[4];
    char **env;
    size_t n = 0;
    size_t i;
    pid_t pid;
    int rc;

    if (exists("/proc/ham")) {
        printf("launch: HAM is running\n");
        return 0;
    }
    snprintf(ham, sizeof ham, "%s/ham/ham", bindir);
    snprintf(log, sizeof log, "%s/ham.log", bindir);
    snprintf(libpath, sizeof libpath, "LD_LIBRARY_PATH=%s/ham", bindir);
    while (environ[n] != NULL) {
        n++;
    }
    env = calloc(n + 2, sizeof *env);
    if (env == NULL) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        env[i] = environ[i];
    }
    env[n] = libpath;
    argv[0] = ham;
    argv[1] = "-f";
    argv[2] = log;
    argv[3] = NULL;
    rc = posix_spawn(&pid, ham, NULL, NULL, argv, env);
    free(env);
    if (rc != 0) {
        printf("launch: starting %s failed: %s\n", ham, strerror(rc));
        return -1;
    }
    if (!wait_for("/proc/ham", WAIT_UP_NS)) {
        printf("launch: %s started (pid %d) but /proc/ham did not appear in 5 s\n",
               ham, (int)pid);
        return -1;
    }
    printf("launch: started HAM pid %d, log %s\n", (int)pid, log);
    return 0;
}

static void detach_all(int quiet_missing)
{
    int i;

    for (i = 3; i >= 0; i--) {
        if (ham_detach_name(0, g_nodes[i].entity, 0) == 0) {
            printf("launch: detached %s\n", g_nodes[i].entity);
        } else if (!quiet_missing) {
            printf("launch: detaching %s: %s\n", g_nodes[i].entity,
                   strerror(errno));
        }
    }
}

static int run_start(const char *bindir, const char *gap)
{
    const char *q = getenv("SIGNAL_QUIET");
    const char *qflag = (q != NULL && strcmp(q, "1") == 0) ? " -q" : "";
    char line[4][320];
    int i;

    snprintf(line[0], sizeof line[0], "%s/sink -o %s%s 64 %s", bindir,
             g_nodes[0].log, qflag, gap);
    snprintf(line[1], sizeof line[1], "%s/relay -o %s%s signal-relay-b signal-sink",
             bindir, g_nodes[1].log, qflag);
    snprintf(line[2], sizeof line[2],
             "%s/relay -o %s%s signal-relay-a signal-relay-b", bindir,
             g_nodes[2].log, qflag);
    snprintf(line[3], sizeof line[3], "%s/source -o %s%s live signal-relay-a",
             bindir, g_nodes[3].log, qflag);

    if (start_ham(bindir) != 0) {
        return 2;
    }
    if (ham_connect(0) == -1) {
        printf("launch: ham_connect failed: %s\n", strerror(errno));
        return 2;
    }
    /* Entities left by an earlier run would restart old nodes. */
    detach_all(1);
    for (i = 0; i < 4; i++) {
        ham_entity_t *ent;
        ham_condition_t *cond;
        ham_action_t *act;
        int64_t t0 = now_ns();

        ent = ham_attach(g_nodes[i].entity, 0, -1, line[i], 0);
        if (ent == NULL) {
            printf("launch: ham_attach %s failed: %s\n", g_nodes[i].entity,
                   strerror(errno));
            ham_disconnect(0);
            return 1;
        }
        cond = ham_condition(ent, CONDDEATH, "death", HREARMAFTERRESTART);
        if (cond == NULL) {
            printf("launch: ham_condition %s failed: %s\n", g_nodes[i].entity,
                   strerror(errno));
            ham_disconnect(0);
            return 1;
        }
        act = ham_action_restart(cond, "restart", line[i], HREARMAFTERRESTART);
        if (act == NULL) {
            printf("launch: ham_action_restart %s failed: %s\n",
                   g_nodes[i].entity, strerror(errno));
            ham_disconnect(0);
            return 1;
        }
        if (!wait_for(g_nodes[i].ready_path, WAIT_UP_NS)) {
            printf("launch: %s did not show %s within 5 s\n", g_nodes[i].entity,
                   g_nodes[i].ready_path);
            ham_disconnect(0);
            return 1;
        }
        printf("launch: %s up after %lld us: %s\n", g_nodes[i].entity,
               (long long)((now_ns() - t0) / 1000), line[i]);
        fflush(stdout);
        ham_action_handle_free(act);
        ham_condition_handle_free(cond);
        ham_entity_handle_free(ent);
    }
    ham_disconnect(0);
    return 0;
}

static int run_stop(void)
{
    if (!exists("/proc/ham")) {
        printf("launch: HAM is not running; nothing to detach\n");
        return 0;
    }
    if (ham_connect(0) == -1) {
        printf("launch: ham_connect failed: %s\n", strerror(errno));
        return 2;
    }
    detach_all(0);
    ham_disconnect(0);
    return 0;
}

/* The last "...: pid P start_ns T" line of a log. 0 when there is none. */
static int last_start(const char *log, pid_t *pid, int64_t *t)
{
    FILE *f = fopen(log, "r");
    char line[512];
    int found = 0;

    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        const char *mark = strstr(line, ": pid ");
        int p;
        long long v;

        if (mark != NULL && sscanf(mark, ": pid %d start_ns %lld", &p, &v) == 2) {
            *pid = (pid_t)p;
            *t = v;
            found = 1;
        }
    }
    fclose(f);
    return found;
}

static int run_kill(const char *log, int count, long pause_ms)
{
    pid_t pid;
    int64_t t_start;
    int64_t best = INT64_MAX;
    int64_t worst = 0;
    int64_t sum = 0;
    int ok = 0;
    int i;

    if (!last_start(log, &pid, &t_start)) {
        printf("launch: no start line in %s\n", log);
        return 2;
    }
    for (i = 1; i <= count; i++) {
        int64_t t_kill;
        int64_t until;
        pid_t next = pid;
        int64_t t_next = t_start;

        nap_ms(pause_ms);
        t_kill = now_ns();
        if (kill(pid, SIGKILL) == -1) {
            printf("launch: kill %d: pid %d: %s\n", i, (int)pid, strerror(errno));
            break;
        }
        until = t_kill + WAIT_RESTART_NS;
        while (now_ns() < until) {
            if (last_start(log, &next, &t_next) && next != pid) {
                break;
            }
            nap_ms(1);
        }
        if (next == pid) {
            printf("launch: kill %d: pid %d killed at_ns %lld, no new start in "
                   "10 s\n", i, (int)pid, (long long)t_kill);
            break;
        }
        printf("launch: kill %d: pid %d killed at_ns %lld, new pid %d start_ns "
               "%lld, kill_to_start_ns %lld\n", i, (int)pid, (long long)t_kill,
               (int)next, (long long)t_next, (long long)(t_next - t_kill));
        fflush(stdout);
        sum += t_next - t_kill;
        if (t_next - t_kill < best) {
            best = t_next - t_kill;
        }
        if (t_next - t_kill > worst) {
            worst = t_next - t_kill;
        }
        ok++;
        pid = next;
        t_start = t_next;
    }
    if (ok > 0) {
        printf("launch: restarted %d of %d, kill_to_start_ns min %lld mean %lld "
               "max %lld\n", ok, count, (long long)best, (long long)(sum / ok),
               (long long)worst);
    } else {
        printf("launch: restarted 0 of %d\n", count);
    }
    return ok == count ? 0 : 1;
}

static int usage(void)
{
    fprintf(stderr, "usage: launch start <bindir> <sink-gap-ms>\n"
                    "       launch stop\n"
                    "       launch kill <log> <count> <pause-ms>\n");
    return 2;
}

int main(int argc, char **argv)
{
    int rc;

    if (argc == 4 && strcmp(argv[1], "start") == 0 && argv[2][0] == '/' &&
        atoi(argv[3]) > 0) {
        rc = run_start(argv[2], argv[3]);
    } else if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        rc = run_stop();
    } else if (argc == 5 && strcmp(argv[1], "kill") == 0 && atoi(argv[3]) > 0 &&
               atoi(argv[4]) >= 0) {
        rc = run_kill(argv[2], atoi(argv[3]), atol(argv[4]));
    } else {
        return usage();
    }
    fflush(stdout);
    return rc;
}
