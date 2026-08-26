/* test_quota_probe_rev.c — 逆序(r6,r7 先行)验证 M2: 配额在目标(接收)侧还是发起侧 */
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
        printf("[QR] === REV order probe: r6,r7 first, then r0..r5 ===\n");

    int order[N] = {6, 7, 0, 1, 2, 3, 4, 5};
    int my_ok_count = 0;
    for (int oi = 0; oi < N; oi++) {
        int token = order[oi];
        for (int p = 0; p < N; p++) {
            if (token == p || is_die_pair(token, p))
                continue;
            MPI_Barrier(MPI_COMM_WORLD);
            if (token == rank) {
                int32_t r = aclrtDeviceEnablePeerAccess(p, 0);
                int ok = (r == ACL_ERROR_NONE);
                if (ok) my_ok_count++;
                printf("[QR] seq: r%d -> dev%d : %s (ret=%d)%s\n",
                       token, p, ok ? "OK  " : "FAIL", r,
                       ok ? "" : "   <== rejected");
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    {
        int ok_by_src[N];
        memset(ok_by_src, 0, sizeof(ok_by_src));
        ok_by_src[rank] = my_ok_count;
        int gsrc[N];
        MPI_Allreduce(ok_by_src, gsrc, N, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        if (rank == 0) {
            int total = 0;
            printf("[QR] === summary ===\n");
            for (int r = 0; r < N; r++) {
                printf("[QR] r%d(dev%d,card%d) initiated %d enables\n",
                       r, r, r / 2, gsrc[r]);
                total += gsrc[r];
            }
            printf("[QR] total OK = %d / 36\n", total);
        }
    }

    for (int p = 0; p < N; p++) {
        if (p != rank && !is_die_pair(rank, p))
            aclrtDeviceDisablePeerAccess(p);
    }
    aclrtResetDevice(dev);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
