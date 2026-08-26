/* 验证方向性: p6→p7 vs p7→p6 */
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

    int tnum = env_int("T", 70);
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

    if (rank == 0) {
        for (int p = 1; p < N; p++) {
            int die = is_die_pair(0, p);
            uint32_t flag = die ? ACL_RT_IPC_MEM_IMPORT_FLAG_DEFAULT : ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS;
            void *pv = NULL;
            ret = aclrtIpcMemImportByKey(&pv, keys[p], flag);
            printf("  r0 import p%d: %s\n", p, ret_str(ret));
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 1) {
        switch (tnum) {
        case 70: /* p6 → p7 */
            printf("  r1: p6 then p7\n");
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[6], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p6: %s\n", ret_str(ret));
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[7], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p7: %s\n", ret_str(ret));
            break;
        case 71: /* p7 → p6 */
            printf("  r1: p7 then p6\n");
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[7], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p7: %s\n", ret_str(ret));
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[6], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p6: %s\n", ret_str(ret));
            break;
        case 72: /* p4 → p5 */
            printf("  r1: p4 then p5\n");
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[4], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p4: %s\n", ret_str(ret));
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[5], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p5: %s\n", ret_str(ret));
            break;
        case 73: /* p5 → p4 */
            printf("  r1: p5 then p4\n");
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[5], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p5: %s\n", ret_str(ret));
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[4], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p4: %s\n", ret_str(ret));
            break;
        case 74: /* p2 → p3 */
            printf("  r1: p2 then p3\n");
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[2], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p2: %s\n", ret_str(ret));
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[3], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p3: %s\n", ret_str(ret));
            break;
        case 75: /* p3 → p2 */
            printf("  r1: p3 then p2\n");
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[3], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p3: %s\n", ret_str(ret));
            ret = aclrtIpcMemImportByKey(&(void*){NULL}, keys[2], ACL_RT_IPC_MEM_IMPORT_FLAG_ENABLE_PEER_ACCESS);
            printf("    p2: %s\n", ret_str(ret));
            break;
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
