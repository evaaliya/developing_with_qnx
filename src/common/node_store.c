/* -std=c11 hides the POSIX calls below unless a feature macro asks. */
#if defined(__QNX__)
#define _QNX_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "node_store.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int node_store_remove(const char *name)
{
    if (name == NULL || name[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (shm_unlink(name) == -1 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

static void *map_fd(int fd, size_t size)
{
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved;

    if (base == MAP_FAILED) {
        saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    /* The mapping keeps the object; the descriptor is not needed. */
    close(fd);
    return base;
}

static void *create_new(const char *name, size_t size)
{
    int fd;
    void *base;

    if (node_store_remove(name) != 0) {
        return NULL;
    }
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd == -1) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)size) == -1) {
        int saved = errno;

        close(fd);
        (void)shm_unlink(name);
        errno = saved;
        return NULL;
    }
    base = map_fd(fd, size);
    if (base == NULL) {
        int saved = errno;

        (void)shm_unlink(name);
        errno = saved;
        return NULL;
    }
    /* A new object reads as zeros; clear it anyway in case it does not. */
    memset(base, 0, size);
    return base;
}

void *node_store_map(const char *name, size_t size, int resume,
                     enum node_store_found *found)
{
    struct stat st;
    int fd;

    if (name == NULL || name[0] != '/' || strchr(name + 1, '/') != NULL ||
        size == 0 || found == NULL) {
        errno = EINVAL;
        return NULL;
    }
    *found = NODE_STORE_NEW;
    if (!resume) {
        return create_new(name, size);
    }
    fd = shm_open(name, O_RDWR, 0);
    if (fd == -1) {
        if (errno != ENOENT) {
            return NULL;
        }
        *found = NODE_STORE_MISSING;
        return create_new(name, size);
    }
    if (fstat(fd, &st) == -1) {
        int saved = errno;

        close(fd);
        errno = saved;
        return NULL;
    }
    /*
     * macOS rounds the object up to a page, so only a short object is
     * the wrong size here. Each book also checks its own size field.
     */
    if (st.st_size < 0 || (size_t)st.st_size < size) {
        close(fd);
        *found = NODE_STORE_WRONG_SIZE;
        return create_new(name, size);
    }
    *found = NODE_STORE_KEPT;
    return map_fd(fd, size);
}

int node_store_unmap(void *base, size_t size)
{
    if (base == NULL) {
        return 0;
    }
    return munmap(base, size);
}

const char *node_store_found_text(enum node_store_found found)
{
    switch (found) {
    case NODE_STORE_NEW:
        return "new books";
    case NODE_STORE_KEPT:
        return "books kept";
    case NODE_STORE_MISSING:
        return "no saved books";
    case NODE_STORE_WRONG_SIZE:
        return "saved books of another size";
    }
    return "unknown";
}
