#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ascend_hal.h"

typedef drvError_t (*create_fn)(drv_mem_handle_t **, size_t, const struct drv_mem_prop *, uint64_t);
static create_fn original;
static drvError_t (*original_import)(uint64_t, uint32_t, drv_mem_handle_t **);
static __typeof__(&halMemExportToShareableHandle) original_export;
static struct {
    drv_mem_handle_t *handle;
    unsigned device;
    unsigned memory_type;
} allocations[64];
static unsigned allocation_count;
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
    if (ret == DRV_ERROR_NONE && allocation_count < 64) {
        allocations[allocation_count].handle = *handle;
        allocations[allocation_count].device = prop->devid;
        allocations[allocation_count++].memory_type = prop->mem_type;
    }
    return ret;
}

static drvError_t import_handle(uint64_t shared, uint32_t device, drv_mem_handle_t **handle)
{
    fprintf(stderr, "TEST_HAL_IMPORT shared=%llu device=%u\n", (unsigned long long)shared, device);
    return original_import(shared, device, handle);
}

static drvError_t export_handle(drv_mem_handle_t *handle, drv_mem_handle_type type, uint64_t flags, uint64_t *shared)
{
    drvError_t ret = original_export(handle, type, flags, shared);
    for (unsigned i = 0; ret == DRV_ERROR_NONE && i < allocation_count; ++i) {
        if (allocations[i].handle == handle) {
            fprintf(stderr, "TEST_HAL_EXPORT shared=%llu device=%u type=%u\n", (unsigned long long)*shared,
                    allocations[i].device, allocations[i].memory_type);
            break;
        }
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
    if (value && strcmp(symbol, "halMemImportFromShareableHandle") == 0) {
        original_import = (void *)value;
        return (void *)import_handle;
    }
    if (value && strcmp(symbol, "halMemExportToShareableHandle") == 0) {
        original_export = (void *)value;
        return (void *)export_handle;
    }
    return value;
}
