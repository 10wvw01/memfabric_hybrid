/* 01_base_memcpy — 单进程设备内存与拷贝基线 (310P3/CANN 9.1.0)
 * DEV 环境变量指定设备号(默认 0)。pattern=(tag<<24)|idx,每步不同 tag 防残留假阳性。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "acl/acl.h"

#define DEV_MEM_SIZE (32u * 1024u * 1024u)  /* 32 MB */
#define BW_SIZE      (16u * 1024u * 1024u)  /* 16 MB */
#define BW_WARMUP    3
#define BW_ITERS     30
#define SWEEP_BUF_SIZE (256u * 1024u * 1024u)  /* 256 MB */

#define TAG_STEP2 0xA2u
#define TAG_STEP4 0xA4u
#define TAG_STEP5 0xA5u

static int g_fail = 0;

static uint32_t pat(uint32_t tag, size_t idx)
{
    return (tag << 24) | ((uint32_t)idx & 0x00FFFFFFu);
}

static void fill_pat(uint32_t *buf, size_t words, uint32_t tag)
{
    for (size_t i = 0; i < words; i++)
        buf[i] = pat(tag, i);
}

static int check_pat(const uint32_t *buf, size_t words, uint32_t tag)
{
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != pat(tag, i)) {
            printf("  [verify] mismatch at word %zu (off 0x%zx): got 0x%08x, want 0x%08x\n",
                   i, i * sizeof(uint32_t), buf[i], pat(tag, i));
            return -1;
        }
    }
    return 0;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void step_line(int n, const char *desc, int ok, int ret)
{
    printf("[01] step %d: %-58s %s (ret=%d)\n", n, desc, ok ? "OK" : "FAIL", ret);
    fflush(stdout);
    if (!ok)
        g_fail = 1;
}

static void bw_line(const char *name, double sec, const char *note)
{
    double gb = (double)BW_ITERS * (double)BW_SIZE / 1e9;
    printf("[01] bw   %-9s: %7.2f GB/s   (%s, total %.3f s)\n",
           name, gb / sec, note, sec);
}

static double bw_of(size_t bytes, double sec)
{
    return (double)BW_ITERS * (double)bytes / 1e9 / sec;
}

int main(void)
{
    const char *dev_env = getenv("DEV");
    int32_t dev = dev_env ? (int32_t)atoi(dev_env) : 0;

    aclrtStream stream = NULL;
    void *da = NULL, *db = NULL, *hp = NULL, *da2 = NULL, *db2 = NULL;
    uint32_t *hbuf = NULL, *vbuf = NULL;
    size_t words = DEV_MEM_SIZE / sizeof(uint32_t);
    int32_t ret;
    int init_ok = 0, dev_ok = 0;

    printf("[01] === 01_base_memcpy: single-proc device mem & memcpy baseline ===\n");
    printf("[01] DEV=%d, dev buf %u MB x2, host buf %u MB, pattern=(tag<<24)|idx\n",
           dev, DEV_MEM_SIZE >> 20, DEV_MEM_SIZE >> 20);

    /* ---- init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[01] init: aclInit FAIL (ret=%d)\n", ret);
        goto out;
    }
    init_ok = 1;
    ret = aclrtSetDevice(dev);
    if (ret != ACL_ERROR_NONE) {
        printf("[01] init: aclrtSetDevice(%d) FAIL (ret=%d)\n", dev, ret);
        goto out;
    }
    dev_ok = 1;
    ret = aclrtCreateStream(&stream);
    if (ret != ACL_ERROR_NONE) {
        printf("[01] init: aclrtCreateStream FAIL (ret=%d)\n", ret);
        goto out;
    }
    printf("[01] init: aclInit / aclrtSetDevice(%d) / aclrtCreateStream OK (ret=0)\n", dev);

    hbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
    vbuf = (uint32_t *)malloc(DEV_MEM_SIZE);
    if (!hbuf || !vbuf) {
        printf("[01] init: host malloc 2x32MB FAIL\n");
        goto out;
    }

    /* ---- step 1: device memory malloc ---- */
    ret = aclrtMalloc(&da, DEV_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
    step_line(1, "aclrtMalloc 32MB dev mem (ACL_MEM_MALLOC_HUGE_FIRST)",
              ret == ACL_ERROR_NONE, ret);
    if (ret != ACL_ERROR_NONE)
        goto out;

    /* ---- step 2: H2D write pattern + D2H readback verify (pageable host) ---- */
    {
        int ok = 0;
        fill_pat(hbuf, words, TAG_STEP2);
        // 这里面使用的是 pageable buffer
        // 驱动内部实际做了: pageable buffer --- copy -> 驱动内部的 staging buf (pinned, DMA 可用) ---> device 显存
        // 两次拷贝:host→host(中转) + host→device(DMA)。
        // 而对于 pinned buffer 使用aclrtMemcpy只有一次拷贝:host→device(DMA 直达)
        // 对比的是 step 5
        ret = aclrtMemcpy(da, DEV_MEM_SIZE, hbuf, DEV_MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret == ACL_ERROR_NONE) {
            memset(vbuf, 0, DEV_MEM_SIZE);
            ret = aclrtMemcpy(vbuf, DEV_MEM_SIZE, da, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, words, TAG_STEP2) == 0);
        }
        step_line(2, "H2D write pattern + D2H readback verify (pageable host)", ok, ret);
        if (ret != ACL_ERROR_NONE)
            goto out;
    }

    /* ---- step 3: same-device D2D sync + D2H verify ---- */
    {
        int ok = 0;
        ret = aclrtMalloc(&db, DEV_MEM_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpy(db, DEV_MEM_SIZE, da, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE);
        if (ret == ACL_ERROR_NONE) {
            memset(vbuf, 0, DEV_MEM_SIZE);
            ret = aclrtMemcpy(vbuf, DEV_MEM_SIZE, db, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, words, TAG_STEP2) == 0);
        }
        step_line(3, "D2D sync bufA->bufB same-dev (aclrtMemcpy) + D2H verify", ok, ret);
        if (ret != ACL_ERROR_NONE)
            goto out;
    }

    /* ---- step 4: same-device D2D async (stream) + D2H verify ---- */
    {
        int ok = 0;
        fill_pat(hbuf, words, TAG_STEP4);
        ret = aclrtMemcpy(da, DEV_MEM_SIZE, hbuf, DEV_MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtMemcpyAsync(db, DEV_MEM_SIZE, da, DEV_MEM_SIZE,
                                   ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        if (ret == ACL_ERROR_NONE)
            ret = aclrtSynchronizeStream(stream);
        if (ret == ACL_ERROR_NONE) {
            memset(vbuf, 0, DEV_MEM_SIZE);
            ret = aclrtMemcpy(vbuf, DEV_MEM_SIZE, db, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat(vbuf, words, TAG_STEP4) == 0);
        }
        step_line(4, "D2D async bufA->bufB same-dev (MemcpyAsync+StreamSync) + D2H verify", ok, ret);
        if (ret != ACL_ERROR_NONE)
            goto out;
    }

    /* ---- step 5: aclrtMallocHost + H2D/D2H roundtrip (pinned host) ---- */
    {
        int ok = 0;
        // 之前使用的是 malloc() 分配的 pageable 内存，现在使用 aclrtMallocHost() 分配 pinned 内存
        // 主要区别是: pageable 类型的内存可以换出到 swap，虚拟->物理映射会变，而pinned内存禁止换出，虚拟->物理映射固定不变
        // - pageable 内存:物理地址可能随时被 OS 换出/重映射 → 驱动必须先用一块内部 pinned 缓冲区做中转(host→staging→device,两次拷贝)
        // - pinned 内存:物理地址永久固定 → DMA 可直接读写(一次拷贝,无中转)
        // 对比的是 step 2
        ret = aclrtMallocHost(&hp, DEV_MEM_SIZE);
        if (ret == ACL_ERROR_NONE) {
            fill_pat((uint32_t *)hp, words, TAG_STEP5);
            ret = aclrtMemcpy(da, DEV_MEM_SIZE, hp, DEV_MEM_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        }
        if (ret == ACL_ERROR_NONE) {
            memset(hp, 0, DEV_MEM_SIZE);
            ret = aclrtMemcpy(hp, DEV_MEM_SIZE, da, DEV_MEM_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
            if (ret == ACL_ERROR_NONE)
                ok = (check_pat((uint32_t *)hp, words, TAG_STEP5) == 0);
        }
        step_line(5, "aclrtMallocHost 32MB + H2D/D2H roundtrip (pinned host)", ok, ret);
        if (ret != ACL_ERROR_NONE)
            goto out;
    }

    /* ---- step 6: bandwidth (only after all correctness passed) ---- */
    if (g_fail) {
        printf("[01] step 6: bandwidth report SKIPPED (correctness not passed)\n");
    } else {
        double t0, t1;

        printf("[01] step 6: bandwidth, %u MB x %d iters, %d warmup, correctness passed\n",
               BW_SIZE >> 20, BW_ITERS, BW_WARMUP);

        /* H2D: pinned host -> dev, 30 sync calls timed */
        for (int i = 0; i < BW_WARMUP; i++)
            aclrtMemcpy(da, BW_SIZE, hp, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        t0 = now_sec();
        for (int i = 0; i < BW_ITERS; i++)
            aclrtMemcpy(da, BW_SIZE, hp, BW_SIZE, ACL_MEMCPY_HOST_TO_DEVICE);
        t1 = now_sec();
        bw_line("H2D", t1 - t0, "30 sync aclrtMemcpy H2D, pinned host");

        /* D2H: dev -> pinned host, 30 sync calls timed */
        for (int i = 0; i < BW_WARMUP; i++)
            aclrtMemcpy(hp, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        t0 = now_sec();
        for (int i = 0; i < BW_ITERS; i++)
            aclrtMemcpy(hp, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_HOST);
        t1 = now_sec();
        bw_line("D2H", t1 - t0, "30 sync aclrtMemcpy D2H, pinned host");

        /* D2D sync: bufA -> bufB (same dev), 30 sync calls timed */
        for (int i = 0; i < BW_WARMUP; i++)
            aclrtMemcpy(db, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE);
        t0 = now_sec();
        for (int i = 0; i < BW_ITERS; i++)
            aclrtMemcpy(db, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE);
        t1 = now_sec();
        bw_line("D2D-sync", t1 - t0, "30 sync aclrtMemcpy D2D, same dev");

        /* D2D async: bufA -> bufB (same dev), 30 submits + 1 final stream sync */
        for (int i = 0; i < BW_WARMUP; i++) {
            aclrtMemcpyAsync(db, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
            aclrtSynchronizeStream(stream);
        }
        t0 = now_sec();
        for (int i = 0; i < BW_ITERS; i++)
            aclrtMemcpyAsync(db, BW_SIZE, da, BW_SIZE, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
        aclrtSynchronizeStream(stream);
        t1 = now_sec();
        bw_line("D2D-async", t1 - t0, "30 MemcpyAsync submits + 1 final StreamSync");

        step_line(6, "bandwidth H2D/D2H/D2D-sync/D2D-async (see lines above)", 1, 0);

        /* ---- step 7: D2D path difference sweep (sync vs async-pipe vs async-serial) ---- */
        {
            static const size_t sweep_sz[] = {
                4u  * 1024u * 1024u,
                16u * 1024u * 1024u,
                64u * 1024u * 1024u,
                256u * 1024u * 1024u
            };
            int sweep_n = (int)(sizeof(sweep_sz) / sizeof(sweep_sz[0]));
            double t0, t1;

            printf("[01] step 7: D2D path sweep — sync(drvMemcpy) vs async-pipe(SDMA) vs async-serial\n");
            printf("[01] %9s  %10s  %12s  %14s\n", "size", "sync", "async-pipe", "async-serial");

            ret = aclrtMalloc(&da2, SWEEP_BUF_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                step_line(7, "aclrtMalloc 256MB x2 for D2D sweep", 0, ret);
                goto out;
            }
            ret = aclrtMalloc(&db2, SWEEP_BUF_SIZE, ACL_MEM_MALLOC_HUGE_FIRST);
            if (ret != ACL_ERROR_NONE) {
                step_line(7, "aclrtMalloc 256MB x2 for D2D sweep", 0, ret);
                goto out;
            }

            for (int s = 0; s < sweep_n; s++) {
                size_t sz = sweep_sz[s];
                double bw_sync, bw_pipe, bw_serial;

                for (int i = 0; i < BW_WARMUP; i++)
                    aclrtMemcpy(db2, sz, da2, sz, ACL_MEMCPY_DEVICE_TO_DEVICE);
                t0 = now_sec();
                for (int i = 0; i < BW_ITERS; i++)
                    aclrtMemcpy(db2, sz, da2, sz, ACL_MEMCPY_DEVICE_TO_DEVICE);
                t1 = now_sec();
                bw_sync = bw_of(sz, t1 - t0);

                for (int i = 0; i < BW_WARMUP; i++) {
                    aclrtMemcpyAsync(db2, sz, da2, sz, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                    aclrtSynchronizeStream(stream);
                }
                t0 = now_sec();
                for (int i = 0; i < BW_ITERS; i++)
                    aclrtMemcpyAsync(db2, sz, da2, sz, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                aclrtSynchronizeStream(stream);
                t1 = now_sec();
                bw_pipe = bw_of(sz, t1 - t0);

                for (int i = 0; i < BW_WARMUP; i++) {
                    aclrtMemcpyAsync(db2, sz, da2, sz, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                    aclrtSynchronizeStream(stream);
                }
                t0 = now_sec();
                for (int i = 0; i < BW_ITERS; i++) {
                    aclrtMemcpyAsync(db2, sz, da2, sz, ACL_MEMCPY_DEVICE_TO_DEVICE, stream);
                    aclrtSynchronizeStream(stream);
                }
                t1 = now_sec();
                bw_serial = bw_of(sz, t1 - t0);

                printf("[01] %6lu MB  %8.2f   %10.2f   %12.2f  GB/s\n",
                       (unsigned long)(sz >> 20), bw_sync, bw_pipe, bw_serial);
            }
            step_line(7, "D2D path sweep sync/async-pipe/async-serial (see lines above)", 1, 0);
        }
    }

out:
    if (hp)
        aclrtFreeHost(hp);
    if (db2)
        aclrtFree(db2);
    if (da2)
        aclrtFree(da2);
    if (db)
        aclrtFree(db);
    if (da)
        aclrtFree(da);
    if (stream)
        aclrtDestroyStream(stream);
    free(hbuf);
    free(vbuf);
    if (dev_ok)
        aclrtResetDevice(dev);
    if (init_ok)
        aclFinalize();

    printf("[01] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
