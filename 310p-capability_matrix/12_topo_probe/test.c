/*
 * 12_topo_probe — 设备拓扑探测(CanAccessPeer / EnablePeerAccess)
 * 机器: Atlas 300I Duo (310P3), CANN 9.1.0, aarch64
 * 说明: 单进程, 不使用 MPI; 遍历 8 个 dev (4 卡 x 2 die, die 对 (0,1)(2,3)(4,5)(6,7))。
 *
 * 步骤:
 *   1. aclInit 后对每个 dev aclrtSetDevice 成功性报告
 *   2. CanAccessPeer 8x8 矩阵 (行=i 访问方, 列=j 被访问方), cell = can 值或 API 错误码
 *   3. EnablePeerAccess 试探矩阵: 对每对 (i,j), i!=j, 在 dev i 上调
 *      aclrtDeviceEnablePeerAccess(j, 0) 记录返回码, 测试成功后立即 disable 避免累积状态污染
 *   4. 汇总: die 对 can=0 / 跨卡 can=1 统计; die 对 enable=507899 / 跨卡 enable=0
 *
 * 隔离策略:
 *   - 先把 CanAccessPeer 8x8 全部查完, 再做 enable 试探, 减少 sticky enable 状态对 can 查询的污染;
 *   - 每对 enable 测试后立即调用 aclrtDeviceDisablePeerAccess, 避免累积状态污染后续测试;
 *   - die 对也调用 enable API, 记录错误码 (预期 507899)。
 *
 * PASS 判定: 矩阵打印成功即 PASS; can 值 / enable 返回码是平台事实, 不算失败。
 */
#include <stdio.h>
#include "acl/acl.h"

#define NDEV 8

/* 507899 = ACL_ERROR_RT_DRV_INTERNAL_ERROR (drv internal error), die 对 enable 常见返回 */
#define ERR_DRV_INTERNAL 507899

static int g_fail = 0;

static int is_die_pair(int i, int j)  /* 同一张卡的两个 die */
{
    return i != j && (i / 2) == (j / 2);
}

static void fmt_can_cell(char *buf, size_t bs, int diag, int32_t ret, int32_t val)
{
    if (diag)
        snprintf(buf, bs, "-");
    else if (ret != ACL_ERROR_NONE)
        snprintf(buf, bs, "E%d", ret);
    else
        snprintf(buf, bs, "%d", val);
}

int main(void)
{
    int32_t can[NDEV][NDEV];
    int32_t canret[NDEV][NDEV];
    int32_t enret[NDEV][NDEV];
    int setdev[NDEV];
    int32_t ret;
    int i, j;
    char cell[32];

    printf("[12] === 12_topo_probe: topology probe (CanAccessPeer / EnablePeerAccess) ===\n");
    printf("[12] 8 dev = 4 card x 2 die, die pairs: (0,1)(2,3)(4,5)(6,7)\n");

    /* ---- init ---- */
    ret = aclInit(NULL);
    if (ret != ACL_ERROR_NONE) {
        printf("[12] init: aclInit FAIL (ret=%d)\n", ret);
        printf("[12] RESULT: FAIL\n");
        return 1;
    }
    printf("[12] init: aclInit OK\n");

    /* ---- step 1: SetDevice on all 8 devices ---- */
    printf("[12] step 1: aclrtSetDevice on all 8 devices\n");
    for (i = 0; i < NDEV; i++) {
        ret = aclrtSetDevice(i);
        setdev[i] = (ret == ACL_ERROR_NONE) ? 1 : 0;
        printf("[12]   aclrtSetDevice(%d): %s (ret=%d)\n", i, setdev[i] ? "OK" : "FAIL", ret);
        if (!setdev[i])
            g_fail = 1;
    }

    /* ---- step 2: CanAccessPeer 8x8 matrix ---- */
    /* 先全部查完 can, 再做 enable 试探, 减少 sticky enable 状态对 can 查询的污染 */
    printf("[12] step 2: aclrtDeviceCanAccessPeer 8x8 matrix (row=i accessor, col=j peer)\n");
    printf("[12]   cell = can value (0/1); 'E<code>' = API error code; '-' = diagonal\n");
    for (i = 0; i < NDEV; i++) {
        if (setdev[i])
            aclrtSetDevice(i);  /* 使 dev i 成为当前设备, 尽量贴近 "current device" 语义 */
        for (j = 0; j < NDEV; j++) {
            if (i == j) {
                can[i][j] = -1;
                canret[i][j] = 0;
                continue;
            }
            canret[i][j] = aclrtDeviceCanAccessPeer(&can[i][j], i, j);
        }
    }
    printf("[12]          ");
    for (j = 0; j < NDEV; j++)
        printf("%8d", j);
    printf("\n");
    for (i = 0; i < NDEV; i++) {
        printf("[12]   i=%d    ", i);
        for (j = 0; j < NDEV; j++) {
            fmt_can_cell(cell, sizeof(cell), i == j, canret[i][j], can[i][j]);
            printf("%8s", cell);
        }
        printf("\n");
    }

    /* ---- step 3: EnablePeerAccess probe matrix ---- */
    /* 每对测试后立即 disable, 避免累积状态污染后续测试 */
    printf("[12] step 3: aclrtDeviceEnablePeerAccess probe matrix (on dev i, enable peer j, flags=0)\n");
    printf("[12]   cell = aclrtDeviceEnablePeerAccess(j,0) return code; '-' = diagonal\n");
    printf("[12]   note: each pair is immediately disabled after test to avoid sticky pollution\n");
    for (i = 0; i < NDEV; i++) {
        if (!setdev[i]) {
            for (j = 0; j < NDEV; j++)
                enret[i][j] = (i == j) ? 0 : ACL_ERROR_INVALID_DEVICE;
            continue;
        }
        aclrtSetDevice(i);
        for (j = 0; j < NDEV; j++) {
            if (i == j) {
                enret[i][j] = 0;
                continue;
            }
            enret[i][j] = aclrtDeviceEnablePeerAccess(j, 0);
            if (enret[i][j] == ACL_ERROR_NONE)
                aclrtDeviceDisablePeerAccess(j);
        }
    }
    printf("[12]          ");
    for (j = 0; j < NDEV; j++)
        printf("%8d", j);
    printf("\n");
    for (i = 0; i < NDEV; i++) {
        printf("[12]   i=%d    ", i);
        for (j = 0; j < NDEV; j++) {
            if (i == j)
                snprintf(cell, sizeof(cell), "-");
            else
                snprintf(cell, sizeof(cell), "%d", enret[i][j]);
            printf("%8s", cell);
        }
        printf("\n");
    }

    /* ---- step 4: summary ---- */
    printf("[12] step 4: summary\n");
    printf("[12]   die pairs (same card, expect can=0):\n");
    for (i = 0; i < NDEV; i += 2) {
        j = i + 1;
        printf("[12]     (%d,%d): can %d->%d=%d, %d->%d=%d | enable %d->%d=%d, %d->%d=%d\n",
               i, j,
               i, j, canret[i][j] == 0 ? can[i][j] : -1,
               j, i, canret[j][i] == 0 ? can[j][i] : -1,
               i, j, enret[i][j],
               j, i, enret[j][i]);
    }
    {
        int cross_total = 0, cross_can1 = 0, cross_can0 = 0, cross_mixed = 0, cross_err = 0;
        int en_ok = 0, en_drv = 0, en_other = 0;
        for (i = 0; i < NDEV; i++) {
            for (j = i + 1; j < NDEV; j++) {
                if (!is_die_pair(i, j)) {
                    int a = (canret[i][j] == 0) ? can[i][j] : -1;
                    int b = (canret[j][i] == 0) ? can[j][i] : -1;
                    cross_total++;
                    if (a == 1 && b == 1)
                        cross_can1++;
                    else if (a == 0 && b == 0)
                        cross_can0++;
                    else if (a == -1 || b == -1)
                        cross_err++;
                    else
                        cross_mixed++;
                }
            }
        }
        for (i = 0; i < NDEV; i++)
            for (j = 0; j < NDEV; j++)
                if (i != j && !is_die_pair(i, j)) {
                    if (enret[i][j] == 0)
                        en_ok++;
                    else if (enret[i][j] == ERR_DRV_INTERNAL)
                        en_drv++;
                    else
                        en_other++;
                }
        printf("[12]   cross-card pairs (i<j): total %d, can=1 both dir: %d, can=0 both dir: %d, mixed: %d, api-err: %d\n",
               cross_total, cross_can1, cross_can0, cross_mixed, cross_err);
        printf("[12]   cross-card enable (ordered): ret=0: %d, ret=%d (drv internal): %d, other: %d\n",
               en_ok, ERR_DRV_INTERNAL, en_drv, en_other);
        if (cross_mixed || cross_err || cross_can0) {
            printf("[12]   cross-card pairs not can=1 both dir:\n");
            for (i = 0; i < NDEV; i++)
                for (j = i + 1; j < NDEV; j++)
                    if (!is_die_pair(i, j)) {
                        int a = (canret[i][j] == 0) ? can[i][j] : -1;
                        int b = (canret[j][i] == 0) ? can[j][i] : -1;
                        if (!(a == 1 && b == 1))
                            printf("[12]     (%d,%d): can %d->%d=%d(ret=%d), %d->%d=%d(ret=%d)\n",
                                   i, j, i, j, a, canret[i][j], j, i, b, canret[j][i]);
                    }
        }
        if (en_other) {
            printf("[12]   cross-card enable cells with unexpected codes:\n");
            for (i = 0; i < NDEV; i++)
                for (j = 0; j < NDEV; j++)
                    if (i != j && !is_die_pair(i, j) &&
                        enret[i][j] != 0 && enret[i][j] != ERR_DRV_INTERNAL)
                        printf("[12]     %d->%d: enable ret=%d\n", i, j, enret[i][j]);
        }
    }

    /* ---- cleanup ---- */
    /* enable 已在每对测试后立即 disable, 无需再次 disable */
    for (i = 0; i < NDEV; i++)
        if (setdev[i])
            aclrtResetDevice(i);
    aclFinalize();

    printf("[12] RESULT: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
