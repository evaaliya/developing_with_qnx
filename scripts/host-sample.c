/*
 * Host side of the guest stall study (F-031): every PERIOD_MS, print the
 * busy share of each Mac core and the CPU time each QEMU thread used in
 * that period. No root needed: host_processor_info for the cores,
 * proc_pidinfo for the threads of a process owned by the same user.
 *
 *   host-sample <qemu-pid> <seconds> [period-ms]
 *
 * One line per period:
 *   s <wall ms> cores <busy %>x<ncores> qemu <thread ms used>... states <r|s|u>...
 * wall ms is CLOCK_REALTIME at the end of the period. Threads are listed
 * in a fixed order (by thread id, as first seen). States: r running,
 * s stopped or waiting, u uninterruptible, other letters as reported.
 */
#include <libproc.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 64
#define MAX_CORES 64

static int64_t wall_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_cores(unsigned *n, uint64_t busy[], uint64_t total[])
{
    natural_t count = 0;
    processor_info_array_t info = NULL;
    mach_msg_type_number_t info_n = 0;
    unsigned i;

    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count,
                            &info, &info_n) != KERN_SUCCESS) {
        return -1;
    }
    if (count > MAX_CORES) {
        count = MAX_CORES;
    }
    for (i = 0; i < count; i++) {
        const integer_t *t = &info[i * CPU_STATE_MAX];
        uint64_t b = (uint64_t)t[CPU_STATE_USER] + (uint64_t)t[CPU_STATE_SYSTEM] +
                     (uint64_t)t[CPU_STATE_NICE];

        busy[i] = b;
        total[i] = b + (uint64_t)t[CPU_STATE_IDLE];
    }
    *n = count;
    vm_deallocate(mach_task_self(), (vm_address_t)info,
                  info_n * sizeof(integer_t));
    return 0;
}

struct qthread {
    uint64_t handle;
    uint64_t cpu_ns;
    int seen;
};

static struct qthread known[MAX_THREADS];
static unsigned known_n;

static int read_threads(pid_t pid, uint64_t used_ns[], char state[])
{
    uint64_t handles[MAX_THREADS];
    int bytes = proc_pidinfo(pid, PROC_PIDLISTTHREADS, 0, handles,
                             (int)sizeof handles);
    unsigned n;
    unsigned i;
    unsigned k;

    if (bytes <= 0) {
        return -1;
    }
    n = (unsigned)bytes / sizeof(uint64_t);
    for (k = 0; k < known_n; k++) {
        known[k].seen = 0;
        used_ns[k] = 0;
        state[k] = '-';
    }
    for (i = 0; i < n; i++) {
        struct proc_threadinfo ti;
        uint64_t cpu;

        if (proc_pidinfo(pid, PROC_PIDTHREADINFO, handles[i], &ti,
                         (int)sizeof ti) != (int)sizeof ti) {
            continue;
        }
        cpu = ti.pth_user_time + ti.pth_system_time;
        for (k = 0; k < known_n; k++) {
            if (known[k].handle == handles[i]) {
                break;
            }
        }
        if (k == known_n) {
            if (known_n >= MAX_THREADS) {
                continue;
            }
            known[k].handle = handles[i];
            known[k].cpu_ns = cpu;
            used_ns[k] = 0;
            known_n++;
        } else {
            /* A reused thread handle can start below the old count. */
            used_ns[k] = cpu >= known[k].cpu_ns ? cpu - known[k].cpu_ns : 0;
            known[k].cpu_ns = cpu;
        }
        known[k].seen = 1;
        switch (ti.pth_run_state) {
        case TH_STATE_RUNNING: state[k] = 'r'; break;
        case TH_STATE_STOPPED: state[k] = 's'; break;
        case TH_STATE_WAITING: state[k] = 'w'; break;
        case TH_STATE_UNINTERRUPTIBLE: state[k] = 'u'; break;
        case TH_STATE_HALTED: state[k] = 'h'; break;
        default: state[k] = '?'; break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    pid_t pid;
    long seconds;
    long period_ms = 20;
    uint64_t busy0[MAX_CORES], total0[MAX_CORES];
    uint64_t busy1[MAX_CORES], total1[MAX_CORES];
    uint64_t used[MAX_THREADS];
    char state[MAX_THREADS];
    unsigned cores = 0;
    int64_t until;
    struct timespec pause;

    if (argc < 3) {
        fprintf(stderr, "usage: host-sample <qemu-pid> <seconds> [period-ms]\n");
        return 2;
    }
    pid = (pid_t)atoi(argv[1]);
    seconds = atol(argv[2]);
    if (argc > 3) {
        period_ms = atol(argv[3]);
    }
    if (pid <= 0 || seconds <= 0 || period_ms <= 0 || period_ms > 1000) {
        fprintf(stderr, "host-sample: bad arguments\n");
        return 2;
    }
    if (read_cores(&cores, busy0, total0) != 0 ||
        read_threads(pid, used, state) != 0) {
        fprintf(stderr, "host-sample: cannot read the cores or the threads of %d\n",
                (int)pid);
        return 1;
    }
    printf("host-sample: pid %d, %u cores, period %ld ms, start %lld ms\n",
           (int)pid, cores, period_ms, (long long)wall_ms());
    fflush(stdout);
    pause.tv_sec = 0;
    pause.tv_nsec = period_ms * 1000000L;
    until = wall_ms() + seconds * 1000;
    while (wall_ms() < until) {
        unsigned i;
        unsigned n;

        nanosleep(&pause, NULL);
        if (read_cores(&n, busy1, total1) != 0 ||
            read_threads(pid, used, state) != 0) {
            fprintf(stderr, "host-sample: reading failed; QEMU gone?\n");
            return 1;
        }
        printf("s %lld cores", (long long)wall_ms());
        for (i = 0; i < n; i++) {
            uint64_t dt = total1[i] - total0[i];
            uint64_t db = busy1[i] - busy0[i];

            printf(" %u", dt ? (unsigned)(db * 100 / dt) : 0u);
            busy0[i] = busy1[i];
            total0[i] = total1[i];
        }
        printf(" qemu");
        for (i = 0; i < known_n; i++) {
            printf(" %.1f", (double)used[i] / 1e6);
        }
        printf(" states ");
        for (i = 0; i < known_n; i++) {
            putchar(state[i]);
        }
        putchar('\n');
    }
    fflush(stdout);
    return 0;
}
