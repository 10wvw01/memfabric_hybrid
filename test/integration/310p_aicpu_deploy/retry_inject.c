#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
static void *(*real_lookup)(void *, const char *);
static int (*launch)(void *, uint32_t, void *, const void *, const void *, size_t, void *, uint32_t);
static int fail_launch(void *a, uint32_t b, void *c, const void *d, const void *e, size_t f, void *g, uint32_t h)
{
    if (getenv("TEST_DEPLOY_CHANNEL_FAIL")) {
        fprintf(stderr, "INJECT KFC launch failure\n");
        return 507000;
    }
    return launch(a, b, c, d, e, f, g, h);
}
void *dlsym(void *handle, const char *name)
{
    if (!real_lookup)
        real_lookup = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    void *p = real_lookup(handle, name);
    if (!strcmp(name, "aclrtLaunchKernelWithHostArgs")) {
        launch = p;
        return (void *)fail_launch;
    }
    return p;
}
