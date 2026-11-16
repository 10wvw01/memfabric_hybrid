#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ascend_hal.h"

typedef drvError_t (*create_fn)(drv_mem_handle_t **, size_t, const struct drv_mem_prop *, uint64_t);
static create_fn original;
static void *(*lookup)(void *, const char *);

static drvError_t create(drv_mem_handle_t **handle, size_t size, const struct drv_mem_prop *prop, uint64_t flag)
{
    const char *selected = getenv("MF_TEST_P2P_OOM_DEVICE");
    if (selected && size == 67108864 && prop->mem_type == MEM_P2P_DDR_TYPE &&
        (strcmp(selected, "all") == 0 || prop->devid == (unsigned)atoi(selected))) {
        fprintf(stderr, "INJECT_P2P_OOM device=%u bytes=%zu\n", prop->devid, size);
        *handle = NULL;
        return DRV_ERROR_OUT_OF_MEMORY;
    }
    drvError_t ret = original(handle, size, prop, flag);
    if (size == 67108864 || size == 4294967296ULL) {
        fprintf(stderr, "TEST_POOL_CREATE device=%u bytes=%zu type=%u ret=%d\n", prop->devid, size, prop->mem_type,
                ret);
    }
    return ret;
}

void *dlsym(void *handle, const char *symbol)
{
    if (!lookup) {
        lookup = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    }
    void *value = lookup(handle, symbol);
    if (value && strcmp(symbol, "halMemCreate") == 0) {
        original = (create_fn)value;
        return (void *)create;
    }
    return value;
}
