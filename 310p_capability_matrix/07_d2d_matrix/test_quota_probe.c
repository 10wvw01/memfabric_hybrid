/*
 * test_quota_probe.c — 精确测定 310P Duo P2P 配额模型
 *
 * 8-rank 实测(见 test_shared_p2p8 输出)之谜:
 *   r0-r5 各发起 6 条 enable 全成功(共 36 条), r6/r7 各 6 条全失败。
 *   若配额是"每卡 8 条关系", card0 参与 r0..r5 的 12 条早该在半路失败。
 *
 * 两种候选模型:
 *   M1 (发起方配额): 每卡最多发起 8 条 enable(direction: 本卡die -> 远端die)
 *   M2 (接收方配额): 每卡最多被 8 条 enable 指向(远端die -> 本卡die)
 *   M3 (关系配额):   每卡最多参与 8 条无序关系(与之前 4-rank T20 观测吻合)
 *
 * test_shared_p2p8 观测: card0 发起 12 条(r0:6 + r1:6)全成功
 *   -> M1 错! (12 > 8 仍成功)
 * r6/r7 发起 12 条全失败, 目标是 card0/1/2
 *   -> 与 M2 吻合: card0 被指向的 enable 已有 r0..r5 中指向它的:
 *      r2->0, r3->0, r4->0, r5->0 (4条) + r2->1, r3->1, r4->1, r5->1 (4条) = 8 条满!
 *      r6->0 是第 9 条 -> FAIL。
 *
 * 本探针: 逐条 enable + 全体同步确认, 打印全局第 k 条 enable 的成败,
 * 找到精确的拒绝点, 验证 M2 (或发现其他模型)。
 *
 * 顺序: 按 (发起rank, 目标peer) 字典序串行:
 *   r0: 2,3,4,5,6,7  r1: 2,3,4,5,6,7  r2: 0,1,4,5,6,7 ... r7: 0,1,2,3,4,5,6
 * 每条 enable 后 Allreduce 确认全员到达, rank0 打印序号与结果。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define N 8

static int is_die_pair(int i, int j)
{
    return i != j && (i / 2) == (j / 2);
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) {
        if (rank == 0) printf("need -np %d\n", N);
        MPI_Finalize();
        return 1;
    }

    int32_t dev = rank;
    setvbuf(stdout, NULL, _IONBF, 0);

    aclInit(NULL);
    aclrtSetDevice(dev);

    if (rank == 0)
        printf("[QP] === quota probe: serial enable in lexicographic order ===\n");

    /* 逐条串行 enable: 外层 token = 发起 rank, 内层 peer 序 */
    int my_ok_count = 0;
    for (int token = 0; token < N; token++) {
        for (int p = 0; p < N; p++) {
            if (token == p || is_die_pair(token, p))
                continue;
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                int32_t r = aclrtDeviceEnablePeerAccess(p, 0);
                int ok = (r == ACL_ERROR_NONE);
                if (ok) my_ok_count++;
                printf("[QP] seq: r%d -> dev%d : %s (ret=%d)%s\n",
                       token, p, ok ? "OK  " : "FAIL", r,
                       ok ? "" : "   <== rejected");
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }

    /* 汇总: 每卡作为【目标】被 enable 的成功数 / 作为【发起方】的成功数 */
    MPI_Barrier(MPI_COMM_WORLD);
    {
        int ok_by_src[N];   /* 发起方统计 */
        memset(ok_by_src, 0, sizeof(ok_by_src));
        ok_by_src[rank] = my_ok_count;
        int gsrc[N];
        MPI_Allreduce(ok_by_src, gsrc, N, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

        if (rank == 0) {
            printf("[QP] === summary ===\n");
            for (int r = 0; r < N; r++)
                printf("[QP] r%d(dev%d,card%d) initiated %d enables\n",
                       r, r, r / 2, gsrc[r]);
            printf("[QP] total OK = %d / 36\n",
                   gsrc[0]+gsrc[1]+gsrc[2]+gsrc[3]+gsrc[4]+gsrc[5]+gsrc[6]+gsrc[7]);
        }
    }

    /* cleanup: disable 掉自己成功的 enable */
    /* 注意: 本探针没有跟踪每条的成功与否到 cleanup 阶段 -> 重新逐条 disable(忽略失败) */
    for (int p = 0; p < N; p++) {
        if (p != rank && !is_die_pair(rank, p))
            aclrtDeviceDisablePeerAccess(p);
    }
    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
