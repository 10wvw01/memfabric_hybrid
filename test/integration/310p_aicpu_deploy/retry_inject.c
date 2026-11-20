#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
static void *(*real_lookup)(void *, const char *);
static int (*real_sync)(void *);
static void *copy_stream;
static int (*launch)(void *, uint32_t, void *, const void *, const void *, size_t, void *, uint32_t);
static int fail_launch(void *a, uint32_t b, void *c, const void *d, const void *e, size_t f, void *g, uint32_t h)
{
    uint64_t expectedSize = 0;
    /* 部署校验参数包含四个 uint64_t 与 256 字节路径；清理校验的 expectedSize 为 0。 */
    if (e && f == 4 * sizeof(uint64_t) + 256)
        memcpy(&expectedSize, (const char *)e + sizeof(uint64_t), sizeof(expectedSize));
    if (getenv("TEST_DEPLOY_CHANNEL_FAIL") || (getenv("TEST_DEPLOY_VERIFY_FAIL") && expectedSize != 0)) {
        fprintf(stderr, "INJECT KFC launch failure\n");
        return 507000;
    }
    if (getenv("TEST_DEPLOY_VERIFY_FAIL") && e && f > 4 && !strncmp((const char *)e, "cat ", 4))
        copy_stream = c;
    return launch(a, b, c, d, e, f, g, h);
}
static int sync_copy_result(void *stream)
{
    int ret = real_sync(stream);
    if (stream == copy_stream) {
        copy_stream = NULL;
        if (ret == 0) {
            fprintf(stderr, "INJECT copy sync result unconfirmed\n");
            return 507018;
        }
    }
    return ret;
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
    if (!strcmp(name, "aclrtSynchronizeStream")) {
        real_sync = p;
        return (void *)sync_copy_result;
    }
    return p;
}
