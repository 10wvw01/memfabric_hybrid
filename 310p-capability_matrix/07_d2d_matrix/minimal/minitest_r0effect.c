/* 验证 r0 的 import 是否影响 r1 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <mpi.h>
#include "acl/acl.h"

#define N 8
#define SLAB_SIZE (16u * 1024u * 1024u)
#define IPC_KEY_LEN 65

static int32_t env_int(const char *n, int32_t d) { const char *s = getenv(n); return s ? atoi(s) : d; }
static const char *ret_str(int32_t r) { return r == 0 ? "OK" : (r == 507899 ? "FAIL(507899)" : "FAIL(other)"); }
static int is_die_pair(int i, int j) { return i != j && (i / 2) == (j / 2); }

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank, nproc;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != N) { if (!rank) printf("need -np %d\n", N); MPI_Finalize(); return 1; }

    int tnum = env_int("T", 100);
    int32_t dev = rank;
    void *slab = NULL;
    char mykey[IPC_KEY_LEN] = {0}, keys[N][IPC_KEY_LEN];

    setvbuf(stdout, NULL, _IONBF, 0);
    aclInit(NULL);
    aclrtSetDevice(dev);

    int32_t ret = aclrtMalloc(&slab, SLAB_SIZE, ACL_MEM_MALLOC_HUGE_FIRST_P2P);
    if (ret == ACL_ERROR_NONE)
        aclrtIpcMemGetExportKey(slab, SLAB_SIZE, mykey, IPC_KEY_LEN, ACL_RT_IPC_MEM_EXPORT_FLAG_DISABLE_PID_VALIDATION);

    MPI_Allgather(mykey, IPC_KEY_LEN, MPI_CHAR, &keys[0][0], IPC_KEY_LEN, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Barrier(MPI_COMM_WORLD);

    /* T100: r0 不 import, r1 import p4,p5,p6,p7 */
    /* T101: r0 import p4, r1 import p4,p5,p6,p7 */
    /* T102: r0 import p4,p5, r1 import p4,p5,p6,p7 */
    /* T103: r0 import p4,p5,p6, r1 import p4,p5,p6,p7 */
    /* T104: r0 import p4,p5,p6,p7, r1 import p4,p5,p6,p7 */

    if (rank == 0) {
        int count = 0;
        switch (tnum) {
        case 100: count = 0; break;
        case 101: count = 1; break;
        case 102: count = 2; break;
        case 103: count = 3; break;
        case 104: count = 4; break;
        }
        if (count > 0) {
            printf("  r0: importing p4");
            for (int i = 0; i < count; i++) {
                int p = 4 + i;
                ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[p], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
                if (i > 0) printf(",p%d", p);
            }
            printf("\n");
        } else {
            printf("  r0: no imports\n");
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        printf("  r1: importing p4,p5,p6,p7\n");
        for (int p = 4; p <= 7; p++) {
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[p], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p%d: %s\n", p, ret_str(ret));
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (slab) aclrtFree(slab);
    aclrtIpcMemClose(mykey);
    for (int p = 0; p < N; p++) if (p != rank) aclrtIpcMemClose(keys[p]);
    aclrtResetDevice(dev);
    if (!rank) printf("=== T%d done ===\n", tnum);
    aclFinalize();
    MPI_Finalize();
    return 0;
}
