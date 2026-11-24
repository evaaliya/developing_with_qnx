/* Shared memory books outlive the mapping that wrote them (D-063). */
#include "node_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void expect_u(const char *name, unsigned long long got,
                     unsigned long long want)
{
    if (got == want) {
        printf("PASS %s\n", name);
        return;
    }
    printf("FAIL %s (got %llu, want %llu)\n", name, got, want);
    failures++;
}

int main(void)
{
    char name[32];
    enum node_store_found found = NODE_STORE_KEPT;
    unsigned char *base;
    size_t size = 300000;

    snprintf(name, sizeof name, "/b23-store-%ld", (long)getpid() % 100000);
    (void)node_store_remove(name);

    base = node_store_map(name, size, 0, &found);
    expect_u("new: mapped", base != NULL, 1);
    if (base == NULL) {
        return EXIT_FAILURE;
    }
    expect_u("new: found new", found, NODE_STORE_NEW);
    expect_u("new: zeroed", base[0] == 0 && base[size - 1] == 0, 1);
    base[0] = 0xab;
    base[size - 1] = 0xcd;
    expect_u("unmap", (unsigned)node_store_unmap(base, size), 0);

    base = node_store_map(name, size, 1, &found);
    expect_u("resume: mapped", base != NULL, 1);
    if (base == NULL) {
        return EXIT_FAILURE;
    }
    expect_u("resume: books kept", found, NODE_STORE_KEPT);
    expect_u("resume: first byte kept", base[0], 0xab);
    expect_u("resume: last byte kept", base[size - 1], 0xcd);
    (void)node_store_unmap(base, size);

    base = node_store_map(name, size * 2, 1, &found);
    expect_u("bigger: mapped", base != NULL, 1);
    expect_u("bigger: the short object is replaced", found,
             NODE_STORE_WRONG_SIZE);
    if (base != NULL) {
        expect_u("bigger: zeroed", base[0], 0);
        (void)node_store_unmap(base, size * 2);
    }

    base = node_store_map(name, size, 0, &found);
    expect_u("fresh start: mapped", base != NULL, 1);
    expect_u("fresh start: new", found, NODE_STORE_NEW);
    if (base != NULL) {
        expect_u("fresh start: zeroed", base[0], 0);
        (void)node_store_unmap(base, size);
    }

    expect_u("remove", (unsigned)node_store_remove(name), 0);
    expect_u("remove again is fine", (unsigned)node_store_remove(name), 0);
    base = node_store_map(name, size, 1, &found);
    expect_u("missing: mapped new", base != NULL, 1);
    expect_u("missing: said so", found, NODE_STORE_MISSING);
    if (base != NULL) {
        (void)node_store_unmap(base, size);
    }
    (void)node_store_remove(name);

    expect_u("bad name is refused",
             node_store_map("no-slash", size, 0, &found) == NULL, 1);
    expect_u("found text", strcmp(node_store_found_text(NODE_STORE_MISSING),
                                  "no saved books") == 0, 1);
    if (failures != 0) {
        printf("%d tests failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("all tests passed\n");
    return EXIT_SUCCESS;
}
