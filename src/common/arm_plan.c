/* -std=c11 hides the POSIX calls below unless a feature macro asks. */
#if defined(__QNX__)
#define _QNX_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "arm_plan.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

double arm_plan_event_distance_m(void)
{
    return ARM_PLAN_MARGIN_M + arm_braking_path_m(ARM_SPEED_M_S);
}

static double eye_after(int64_t t_ns)
{
    struct arm_state arm;
    double home[ARM_JOINTS];
    double eye[ARM_JOINTS];

    arm_home_pose(home);
    arm_eye_pose(eye);
    arm_init(&arm, home, 0);
    arm_set_target(&arm, eye);
    arm_step(&arm, t_ns);
    return arm_eye_distance(arm.q);
}

int64_t arm_plan_time_to_distance_ns(double dist_m)
{
    /* The approach from home takes about 0.8 s; 4 s is far beyond it. */
    int64_t lo = 0;
    int64_t hi = 4000000000LL;

    if (eye_after(lo) <= dist_m) {
        return 0;
    }
    if (eye_after(hi) > dist_m) {
        return -1;
    }
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;

        if (eye_after(mid) > dist_m) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return hi;
}

void arm_approach_publish(struct arm_approach *a, int64_t start_ns,
                          int32_t start_eye_um)
{
    if (a == NULL) {
        return;
    }
    __atomic_store_n(&a->magic, ARM_APPROACH_MAGIC, __ATOMIC_RELAXED);
    __atomic_store_n(&a->start_eye_um, start_eye_um, __ATOMIC_RELAXED);
    __atomic_store_n(&a->start_ns, start_ns, __ATOMIC_RELEASE);
}

int arm_approach_read(const struct arm_approach *a, int64_t *start_ns,
                      int32_t *start_eye_um)
{
    int64_t start;

    if (a == NULL) {
        return -1;
    }
    start = __atomic_load_n(&a->start_ns, __ATOMIC_ACQUIRE);
    if (start == 0) {
        return 0;
    }
    if (__atomic_load_n(&a->magic, __ATOMIC_RELAXED) != ARM_APPROACH_MAGIC) {
        return -1;
    }
    if (start_ns != NULL) {
        *start_ns = start;
    }
    if (start_eye_um != NULL) {
        *start_eye_um = __atomic_load_n(&a->start_eye_um, __ATOMIC_RELAXED);
    }
    return 1;
}

const struct arm_approach *arm_approach_open(const char *name)
{
    struct stat st;
    void *base;
    int fd;
    int saved;

    fd = shm_open(name, O_RDONLY, 0);
    if (fd == -1) {
        return NULL;
    }
    if (fstat(fd, &st) == -1) {
        saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    if (st.st_size < (off_t)sizeof(struct arm_approach)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    base = mmap(NULL, sizeof(struct arm_approach), PROT_READ, MAP_SHARED, fd, 0);
    saved = errno;
    close(fd);
    if (base == MAP_FAILED) {
        errno = saved;
        return NULL;
    }
    return base;
}

void arm_approach_close(const struct arm_approach *a)
{
    if (a != NULL) {
        (void)munmap((void *)(uintptr_t)a, sizeof *a);
    }
}
