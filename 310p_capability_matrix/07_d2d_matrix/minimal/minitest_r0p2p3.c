/* 验证 r0 import p2,p3 是否影响 r1 */
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

    int tnum = env_int("T", 110);
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

    /* T110: r0 不 import, r1 import p4,p5,p6 */
    /* T111: r0 import p2, r1 import p4,p5,p6 */
    /* T112: r0 import p3, r1 import p4,p5,p6 */
    /* T113: r0 import p2,p3, r1 import p4,p5,p6 */
    /* T114: r0 import p1,p2,p3, r1 import p4,p5,p6 */

    if (rank == 0) {
        switch (tnum) {
        case 110: printf("  r0: no imports\n"); break;
        case 111:
            printf("  r0: import p2\n");
            aclrtIpcMemImportByKey(&(void*){NULL}, keys[2], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            break;
        case 112:
            printf("  r0: import p3\n");
            aclrtIpcMemImportByKey(&(void*){NULL}, keys[3], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            break;
        case 113:
            printf("  r0: import p2,p3\n");
            aclrtIpcMemImportByKey(&(void*){NULL}, keys[2], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            aclrtIpcMemImportByKey(&(void*){NULL}, keys[3], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            break;
        case 114:
            printf("  r0: import p1,p2,p3\n");
            aclrtIpcMemImportByKey(&(void*){NULL}, keys[1], ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT);
            aclrtIpcMemImportByKey(&(void*){NULL}, keys[2], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            aclrtIpcMemImportByKey(&(void*){NULL}, keys[3], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            break;
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        printf("  r1: import p4,p5,p6\n");
        ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[4], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("    p4: %s\n", ret_str(ret));
        ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[5], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("    p5: %s\n", ret_str(ret));
        ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[6], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
        printf("    p6: %s\n", ret_str(ret));
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
