#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <mpi.h>
#include "hccl/hccl.h"
#include "acl/acl.h"
#include "smem.h"
#include "smem_shm.h"
#include "hybm_sdma_contract.h"

using namespace ock::mf;

static void Check(const char *name, int ret)
{
    if (ret != 0) {
        std::fprintf(stderr, "%s failed: %d\n", name, ret);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
}

static HcclComm CreateHccl(int rank, int size)
{
    HcclRootInfo root{};
    if (rank == 0)
        Check("HCCL root", HcclGetRootInfo(&root));
    MPI_Bcast(&root, sizeof(root), MPI_BYTE, 0, MPI_COMM_WORLD);
    HcclComm comm = nullptr;
    Check("HCCL init", HcclCommInitRootInfo(size, &root, rank, &comm));
    return comm;
}

static void CheckHccl(HcclComm comm, int rank, int size)
{
    float input = static_cast<float>(rank + 1), output = 0;
    void *buffer = nullptr;
    aclrtStream stream = nullptr;
    Check("HCCL buffer", aclrtMalloc(&buffer, 64, ACL_MEM_MALLOC_HUGE_FIRST));
    Check("HCCL stream", aclrtCreateStream(&stream));
    Check("HCCL input", aclrtMemcpy(buffer, 64, &input, sizeof(input), ACL_MEMCPY_HOST_TO_DEVICE));
    Check("HCCL allreduce", HcclAllReduce(buffer, buffer, 1, HCCL_DATA_TYPE_FP32, HCCL_REDUCE_SUM, comm, stream));
    Check("HCCL sync", aclrtSynchronizeStreamWithTimeout(stream, 5000));
    Check("HCCL result", aclrtMemcpy(&output, sizeof(output), buffer, sizeof(output), ACL_MEMCPY_DEVICE_TO_HOST));
    Check("HCCL sum", output == static_cast<float>(size * (size + 1) / 2) ? 0 : 1);
    aclrtDestroyStream(stream);
    aclrtFree(buffer);
}

#ifndef MF_PYPTO_CALLBACK
static void CheckLease(smem_shm_t pool)
{
    raise(SIGSTOP);
    smem_shm_sdma_status_t result{};
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        Check("read status", smem_shm_sdma_get_status(pool, &result));
        if (result.state == SMEM_SHM_SDMA_FAILED)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("LEASE state=%u code=%u safe=%u\n", result.state, result.error, result.releaseConfirmed);
    std::fflush(stdout);
    Check("host loss",
          result.state == SMEM_SHM_SDMA_FAILED && result.error == 1380 && result.releaseConfirmed == 1 ? 0 : 1);
}

static void RunPoolCycle(unsigned cycle, int rank, int size, const char *mode, aclrtContext caller, HcclComm &comm)
{
    void *gva = nullptr;
    auto pool = smem_shm_create(cycle, size, rank, 32ULL * 1024 * 1024, SMEMS_DATA_OP_SDMA, 0, &gva);
    Check("create", pool && gva ? 0 : 1);
    if (cycle == 0 && std::strcmp(mode, "hccl-after") == 0)
        comm = CreateHccl(rank, size);
    if (comm != nullptr)
        CheckHccl(comm, rank, size);
    aclrtContext current = nullptr;
    Check("current", aclrtGetCurrentContext(&current));
    Check("context restored", current == caller ? 0 : 1);
    auto begin = std::chrono::steady_clock::now();
    Check("business synchronization", aclrtSynchronizeDeviceWithTimeout(1000));
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
    auto *workspace = smem_shm_sdma_get_workspace(pool);
    Check("workspace", workspace ? 0 : 1);
    smem_shm_sdma_status_t currentStatus{};
    Check("running status", smem_shm_sdma_get_status(pool, &currentStatus));
    Check("service running", currentStatus.state == SMEM_SHM_SDMA_RUNNING ? 0 : 1);
    MPI_Barrier(MPI_COMM_WORLD);
    std::printf("READY pid=%d cycle=%u businessSyncMs=%ld\n", getpid(), cycle, elapsed);
    std::fflush(stdout);
    if (std::strcmp(mode, "hold") == 0) {
        for (;;)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (std::strcmp(mode, "lease") == 0)
        CheckLease(pool);

    MPI_Barrier(MPI_COMM_WORLD);
    Check("destroy", smem_shm_destroy(pool, 0));
    MPI_Barrier(MPI_COMM_WORLD);
    Check("context after destroy", aclrtGetCurrentContext(&current));
    Check("restored after destroy", current == caller ? 0 : 1);
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int rank = 0, size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    Check("two or four ranks", size == 2 || size == 4 ? 0 : 1);
    const char *mode = argc > 1 ? argv[1] : "normal";
    const int device = std::getenv("TEST_DEVICE_BASE") ? std::atoi(std::getenv("TEST_DEVICE_BASE")) : 6;
    const char *url = std::getenv("STORE_URL");
    Check("aclInit", aclInit(nullptr));
    Check("setDevice", aclrtSetDevice(device + rank));
    aclrtContext explicitContext = nullptr;
    if (std::strcmp(mode, "explicit") == 0)
        Check("create explicit business context", aclrtCreateContext(&explicitContext, device + rank));
    aclrtContext caller = nullptr;
    Check("caller", aclrtGetCurrentContext(&caller));
    HcclComm comm = nullptr;
    if (std::strcmp(mode, "hccl-first") == 0)
        comm = CreateHccl(rank, size);
    Check("smem_init", smem_init(0));
    smem_shm_config_t cfg;
    Check("config", smem_shm_config_init(&cfg));
    cfg.startConfigStoreServer = rank == 0;
    Check("shm_init", smem_shm_init(url ? url : "tcp://127.0.0.1:18937", size, rank, device + rank, &cfg));
    for (unsigned cycle = 0; cycle < 3; ++cycle)
        RunPoolCycle(cycle, rank, size, mode, caller, comm);
    if (comm != nullptr)
        Check("HCCL destroy", HcclCommDestroy(comm));
    smem_shm_uninit(0);
    smem_uninit();
    if (explicitContext != nullptr)
        Check("destroy explicit business context", aclrtDestroyContext(explicitContext));
    Check("reset", aclrtResetDevice(device + rank));
    Check("finalize", aclFinalize());
    MPI_Finalize();
    return 0;
}
#else
extern "C" int mf_pypto_coexist(int (*callback)(), int order)
{
    MPI_Init(nullptr, nullptr);
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    int device = 0;
    Check("device", aclrtGetDevice(&device));
    aclrtContext caller = nullptr;
    Check("caller context", aclrtGetCurrentContext(&caller));
    if (order == 0)
        Check("PyPTO before MF", callback());
    Check("smem init", smem_init(0));
    smem_shm_config_t cfg{};
    Check("config", smem_shm_config_init(&cfg));
    cfg.startConfigStoreServer = rank == 0;
    Check("shm init", smem_shm_init(std::getenv("STORE_URL"), 2, rank, device, &cfg));
    void *gva = nullptr;
    auto pool = smem_shm_create(0, 2, rank, 32ULL * 1024 * 1024, SMEMS_DATA_OP_SDMA, 0, &gva);
    Check("pool", pool && gva ? 0 : 1);
    Check("PyPTO while MF runs", callback());
    smem_shm_sdma_status_t status{};
    Check("service status", smem_shm_sdma_get_status(pool, &status));
    Check("service running", status.state == SMEM_SHM_SDMA_RUNNING ? 0 : 1);
    MPI_Barrier(MPI_COMM_WORLD);
    Check("destroy", smem_shm_destroy(pool, 0));
    smem_shm_uninit(0);
    smem_uninit();
    aclrtContext current = nullptr;
    Check("current context", aclrtGetCurrentContext(&current));
    Check("restored", current == caller ? 0 : 1);
    MPI_Finalize();
    return 0;
}
#endif
