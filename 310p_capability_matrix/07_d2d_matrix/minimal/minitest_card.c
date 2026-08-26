/* 测试不同卡的 die1 是否都有 2 PEER 限制 */
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

    int tnum = env_int("T", 80);
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

    /* T80: card0 die0 (dev0) import 4 PEER */
    /* T81: card0 die1 (dev1) import 4 PEER */
    /* T82: card2 die0 (dev4) import 4 PEER */
    /* T83: card2 die1 (dev5) import 4 PEER */

    int importer = -1, target_start = -1;
    switch (tnum) {
    case 80: importer = 0; target_start = 4; break;  /* dev0 import dev4,5,6,7 */
    case 81: importer = 1; target_start = 4; break;  /* dev1 import dev4,5,6,7 */
    case 82: importer = 4; target_start = 0; break;  /* dev4 import dev0,1,2,3 */
    case 83: importer = 5; target_start = 0; break;  /* dev5 import dev0,1,2,3 */
    }

    if (rank == importer) {
        printf("  r%d(dev%d) importing p%d-p%d:\n", importer, dev, target_start, target_start + 3);
        for (int i = 0; i < 4; i++) {
            int p = target_start + i;
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
