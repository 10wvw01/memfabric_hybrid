/*
 * 310P3 Bare ACL P2P VMM Benchmark (MPI cross-process)
 *
 * rank0 (exporter, devA): alloc VMM P2P memory + export shareable handle + send to rank1
 * rank1 (importer, devB): import + alloc local + D2D/H2D/D2H bandwidth test
 *
 * D2D: devA exported VA -> devB local (Pull mode, cross-process)
 * H2D: host -> devB local VA
 * D2H: devB local VA -> host
 *
 * Usage: mpirun -np 2 ./310p3_aclrt_benchmark [devA] [devB]
 *   devA default 0, devB default 1
 */
#include <iostream>
#include <vector>
#include <sys/time.h>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <mpi.h>
#include "acl/acl.h"

#define CHECK_ACL(x)                                                                        \
    do {                                                                                    \
        aclError __ret = x;                                                                 \
        if (__ret != ACL_ERROR_NONE) {                                                      \
            std::cerr << __FILE__ << ":" << __LINE__ << " aclError:" << __ret << std::endl; \
            return -1;                                                                      \
        }                                                                                   \
    } while (0)

static double GetTimeUs()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (double)tv.tv_sec * 1000000.0 + (double)tv.tv_usec;
}

static double CalcBw(uint64_t bytes, double us)
{
    return (double)bytes / us / 1000.0;
}

static std::string BwStr(uint64_t bytes, double us)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << CalcBw(bytes, us) << " GB/s";
    return oss.str();
}

static std::string BsStr(uint32_t bs)
{
    if (bs >= 1024 * 1024) { return std::to_string(bs / (1024 * 1024)) + "MB"; }
    return std::to_string(bs / 1024) + "KB";
}

struct VmmBuffer
{
    void *va = nullptr;
    aclrtDrvMemHandle handle = nullptr;
    int32_t deviceId = 0;
    size_t size = 0;
};

static VmmBuffer AllocVmmP2P(int32_t deviceId, size_t size, int32_t peerDeviceId = -1)
{
    VmmBuffer buf;
    buf.deviceId = deviceId;
    buf.size = size;

    aclrtPhysicalMemProp prop{};
    prop.handleType = ACL_MEM_HANDLE_TYPE_NONE;
    prop.allocationType = static_cast<aclrtMemAllocationType>(0);
    prop.memAttr = ACL_MEM_P2P_HUGE;
    prop.location.id = deviceId;
    prop.location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
    prop.reserve = 0ULL;

    size_t granularity = 0;
    auto ret = aclrtMemGetAllocationGranularity(&prop, ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM, &granularity);
    if (ret != 0 || granularity == 0) {
        std::cerr << "aclrtMemGetAllocationGranularity failed, ret:" << ret << std::endl;
        return buf;
    }
    size_t alignedSize = (size + granularity - 1) & ~(granularity - 1);

    ret = aclrtMallocPhysical(&buf.handle, alignedSize, &prop, 0);
    if (ret != 0 || buf.handle == nullptr) {
        std::cerr << "aclrtMallocPhysical failed, ret:" << ret << " dev:" << deviceId << " size:" << alignedSize << std::endl;
        return buf;
    }

    ret = aclrtReserveMemAddress(&buf.va, alignedSize, 0, nullptr, 0);
    if (ret != 0 || buf.va == nullptr) {
        std::cerr << "aclrtReserveMemAddress failed, ret:" << ret << std::endl;
        aclrtFreePhysical(buf.handle);
        buf.handle = nullptr;
        return buf;
    }

    ret = aclrtMapMem(buf.va, alignedSize, 0, buf.handle, 0);
    if (ret != 0) {
        std::cerr << "aclrtMapMem failed, ret:" << ret << std::endl;
        aclrtReleaseMemAddress(buf.va);
        aclrtFreePhysical(buf.handle);
        buf.va = nullptr;
        buf.handle = nullptr;
        return buf;
    }

    aclrtMemAccessDesc localDesc{ACL_RT_MEM_ACCESS_FLAGS_READWRITE,
                                {static_cast<uint32_t>(deviceId), ACL_MEM_LOCATION_TYPE_DEVICE}, {}};
    aclrtMemSetAccess(buf.va, alignedSize, &localDesc, 1);

    if (peerDeviceId >= 0 && (peerDeviceId / 2) != (deviceId / 2)) {
        aclrtMemAccessDesc peerDesc{ACL_RT_MEM_ACCESS_FLAGS_READWRITE,
                                    {static_cast<uint32_t>(peerDeviceId), ACL_MEM_LOCATION_TYPE_DEVICE}, {}};
        auto r = aclrtMemSetAccess(buf.va, alignedSize, &peerDesc, 1);
        if (r != 0) {
            std::cerr << "WARN: AclrtMemSetAccess for peer dev:" << peerDeviceId
                      << " failed:" << r << " (non-fatal)" << std::endl;
        }
    }

    return buf;
}

static VmmBuffer ImportVmmP2P(int32_t localDeviceId, int32_t peerDeviceId, uint64_t shareableHandle, size_t size)
{
    VmmBuffer buf;
    buf.deviceId = localDeviceId;
    buf.size = size;

    aclrtSetDevice(localDeviceId);

    auto ret = aclrtReserveMemAddress(&buf.va, size, 0, nullptr, 0);
    if (ret != 0 || buf.va == nullptr) {
        std::cerr << "Import: aclrtReserveMemAddress failed, ret:" << ret << std::endl;
        return buf;
    }

    bool sameCard = (localDeviceId / 2) == (peerDeviceId / 2);
    if (sameCard) {
        aclrtSetDevice(peerDeviceId);
    }

    ret = aclrtMemImportFromShareableHandle(shareableHandle, localDeviceId, &buf.handle);
    if (sameCard) {
        aclrtSetDevice(localDeviceId);
    }
    if (ret != 0 || buf.handle == nullptr) {
        std::cerr << "Import: aclrtMemImportFromShareableHandle failed, ret:" << ret
                  << " local:" << localDeviceId << " peer:" << peerDeviceId << std::endl;
        aclrtReleaseMemAddress(buf.va);
        buf.va = nullptr;
        return buf;
    }

    ret = aclrtMapMem(buf.va, size, 0, buf.handle, 0);
    if (ret != 0) {
        std::cerr << "Import: aclrtMapMem failed, ret:" << ret << std::endl;
        aclrtFreePhysical(buf.handle);
        aclrtReleaseMemAddress(buf.va);
        buf.va = nullptr;
        buf.handle = nullptr;
        return buf;
    }

    aclrtMemAccessDesc impDesc{ACL_RT_MEM_ACCESS_FLAGS_READWRITE,
                               {static_cast<uint32_t>(localDeviceId), ACL_MEM_LOCATION_TYPE_DEVICE}, {}};
    aclrtMemSetAccess(buf.va, size, &impDesc, 1);

    return buf;
}

static void FreeVmmP2P(VmmBuffer &buf)
{
    if (buf.va != nullptr) {
        aclrtUnmapMem(buf.va);
        aclrtReleaseMemAddress(buf.va);
        buf.va = nullptr;
    }
    if (buf.handle != nullptr) {
        aclrtFreePhysical(buf.handle);
        buf.handle = nullptr;
    }
}

static const uint32_t BLOCK_SIZES[] = {
    1024 * 1024, 4 * 1024 * 1024, 16 * 1024 * 1024,
    64 * 1024 * 1024, 128 * 1024 * 1024
};
static const int NUM_SIZES = sizeof(BLOCK_SIZES) / sizeof(BLOCK_SIZES[0]);
static const uint32_t COUNT = 100;
static const uint32_t WARMUP = 10;
static const size_t BUF_SIZE = 256 * 1024 * 1024;

struct ConcResult {
    uint32_t bs;
    double h2dBw;
    double d2hBw;
};

static std::string BwValStr(double bw)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << bw << " GB/s";
    return oss.str();
}

static std::string RatioStr(double ratio)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << ratio << "x";
    return oss.str();
}

static double RunMemcpyLoop(void *dst, void *src, uint32_t bs, aclrtStream stream, bool isH2D)
{
    auto kind = isH2D ? ACL_MEMCPY_HOST_TO_DEVICE : ACL_MEMCPY_DEVICE_TO_HOST;
    const char *dir = isH2D ? "H2D" : "D2H";
    for (int w = 0; w < WARMUP; w++) {
        if (aclrtMemcpyAsync(dst, bs, src, bs, kind, stream) != 0 ||
            aclrtSynchronizeStream(stream) != 0) {
            std::cerr << "conc " << dir << " warmup failed, bs=" << bs << std::endl;
            std::exit(1);
        }
    }
    double t0 = GetTimeUs();
    for (int i = 0; i < COUNT; i++) {
        if (aclrtMemcpyAsync(dst, bs, src, bs, kind, stream) != 0 ||
            aclrtSynchronizeStream(stream) != 0) {
            std::cerr << "conc " << dir << " test failed, bs=" << bs << std::endl;
            std::exit(1);
        }
    }
    return GetTimeUs() - t0;
}

static void RunConcTest(void *devVa, void *hostMem, aclrtStream stream,
                        std::vector<ConcResult> &results)
{
    for (int s = 0; s < NUM_SIZES; s++) {
        uint32_t bs = BLOCK_SIZES[s];
        uint64_t totalBytes = (uint64_t)COUNT * bs;
        double h2dUs = RunMemcpyLoop(devVa, hostMem, bs, stream, true);
        double d2hUs = RunMemcpyLoop(hostMem, devVa, bs, stream, false);
        results.push_back({bs, CalcBw(totalBytes, h2dUs), CalcBw(totalBytes, d2hUs)});
    }
}

static void PrintConcComparison(const std::vector<ConcResult> &r0, const std::vector<ConcResult> &r1,
                                const std::vector<ConcResult> &single, int devA, int devB)
{
    std::cout << std::endl;
    std::cout << "=== Concurrent H2D/D2H (both ranks, single-sync) ===" << std::endl;
    std::cout << std::left
              << std::setw(12) << "BlockSize"
              << std::setw(16) << ("dev" + std::to_string(devA) + "-H2D")
              << std::setw(16) << ("dev" + std::to_string(devB) + "-H2D")
              << std::setw(16) << "agg-H2D"
              << std::setw(12) << "H2D-ratio"
              << std::setw(16) << ("dev" + std::to_string(devA) + "-D2H")
              << std::setw(16) << ("dev" + std::to_string(devB) + "-D2H")
              << std::setw(16) << "agg-D2H"
              << std::setw(12) << "D2H-ratio"
              << std::endl;
    std::cout << std::string(132, '-') << std::endl;
    for (int s = 0; s < NUM_SIZES; s++) {
        double h2dRatio = (single[s].h2dBw > 0) ? r1[s].h2dBw / single[s].h2dBw : 0;
        double d2hRatio = (single[s].d2hBw > 0) ? r1[s].d2hBw / single[s].d2hBw : 0;
        std::cout << std::left << std::setw(12) << BsStr(r0[s].bs)
                  << std::setw(16) << BwValStr(r0[s].h2dBw)
                  << std::setw(16) << BwValStr(r1[s].h2dBw)
                  << std::setw(16) << BwValStr(r0[s].h2dBw + r1[s].h2dBw)
                  << std::setw(12) << RatioStr(h2dRatio)
                  << std::setw(16) << BwValStr(r0[s].d2hBw)
                  << std::setw(16) << BwValStr(r1[s].d2hBw)
                  << std::setw(16) << BwValStr(r0[s].d2hBw + r1[s].d2hBw)
                  << std::setw(12) << RatioStr(d2hRatio)
                  << std::endl;
    }
    std::cout << std::endl;
    std::cout << "H2D-ratio = dev" << devB << "-H2D(concurrent) / dev" << devB
              << "-H2D(single); ~1.0x: no contention, <1.0x: contention" << std::endl;
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    int rank = 0, nproc = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nproc);
    if (nproc != 2) {
        if (rank == 0)
            std::cerr << "FATAL: need 2 MPI procs, got " << nproc << std::endl;
        MPI_Finalize();
        return 1;
    }

    int32_t devA = 0;
    int32_t devB = 1;
    if (argc >= 3) {
        devA = atoi(argv[1]);
        devB = atoi(argv[2]);
    }

    bool sameCard = (devA / 2) == (devB / 2);
    const char *topo = sameCard ? "same-card cross-die" : "cross-card";

    CHECK_ACL(aclInit(nullptr));

    if (rank == 0) {
        /* ===== rank0: exporter (devA) ===== */
        CHECK_ACL(aclrtSetDevice(devA));

        VmmBuffer devABuf = AllocVmmP2P(devA, BUF_SIZE, devB);
        if (devABuf.va == nullptr) {
            std::cerr << "Failed to allocate VMM P2P buffer on dev" << devA << std::endl;
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        std::cout << "[r0] dev" << devA << " VMM P2P buffer: va=" << devABuf.va
                  << " size=" << BUF_SIZE << std::endl;

        uint64_t shareableHandle = 0;
        auto ret = aclrtMemExportToShareableHandle(devABuf.handle, ACL_MEM_HANDLE_TYPE_NONE,
                                                   ACL_RT_VMM_EXPORT_FLAG_DISABLE_PID_VALIDATION, &shareableHandle);
        if (ret != 0 || shareableHandle == 0) {
            std::cerr << "aclrtMemExportToShareableHandle failed, ret:" << ret << std::endl;
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        std::cout << "[r0] exported shareable handle: " << shareableHandle << std::endl;

        MPI_Send(&shareableHandle, 1, MPI_UINT64_T, 1, 100, MPI_COMM_WORLD);

        void *hostMem0 = nullptr;
        if (posix_memalign(&hostMem0, 4096, BUF_SIZE) != 0 || hostMem0 == nullptr) {
            std::cerr << "rank0 posix_memalign failed, size=" << BUF_SIZE << std::endl;
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        aclrtStream stream0;
        CHECK_ACL(aclrtCreateStream(&stream0));

        MPI_Barrier(MPI_COMM_WORLD);

        std::vector<ConcResult> concResults0;
        RunConcTest(devABuf.va, hostMem0, stream0, concResults0);

        MPI_Barrier(MPI_COMM_WORLD);

        MPI_Send(concResults0.data(), NUM_SIZES * sizeof(ConcResult), MPI_BYTE, 1, 200, MPI_COMM_WORLD);
        MPI_Barrier(MPI_COMM_WORLD);

        CHECK_ACL(aclrtDestroyStream(stream0));
        free(hostMem0);
        FreeVmmP2P(devABuf);
        CHECK_ACL(aclrtResetDevice(devA));
        CHECK_ACL(aclFinalize());
        MPI_Finalize();
        return 0;
    }

    /* ===== rank1: importer (devB) ===== */
    uint64_t shareableHandle = 0;
    MPI_Recv(&shareableHandle, 1, MPI_UINT64_T, 0, 100, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (shareableHandle == 0) {
        std::cerr << "[r1] received invalid shareable handle" << std::endl;
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    CHECK_ACL(aclrtSetDevice(devB));

    VmmBuffer imported = ImportVmmP2P(devB, devA, shareableHandle, BUF_SIZE);
    if (imported.va == nullptr) {
        std::cerr << "Failed to import VMM P2P buffer on dev" << devB << std::endl;
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    std::cout << "[r1] dev" << devB << " imported VMM P2P buffer: va=" << imported.va
              << " size=" << BUF_SIZE << std::endl;

    VmmBuffer local = AllocVmmP2P(devB, BUF_SIZE);
    if (local.va == nullptr) {
        std::cerr << "Failed to allocate local VMM P2P buffer on dev" << devB << std::endl;
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    void *hostMem = nullptr;
    posix_memalign(&hostMem, 4096, BUF_SIZE);

    aclrtStream stream;
    CHECK_ACL(aclrtCreateStream(&stream));

    std::cout << std::endl;
    std::cout << "Bare ACL P2P VMM benchmark (MPI cross-process, " << COUNT << " iterations)" << std::endl;
    std::cout << "D2D: dev" << devA << " exported VA -> dev" << devB << " local (Pull, " << topo << ")" << std::endl;
    std::cout << "H2D: host -> dev" << devB << " local VA" << std::endl;
    std::cout << "D2H: dev" << devB << " local VA -> host" << std::endl;
    std::cout << std::endl;

    /* D2D table: imported VA -> local VA, vary offset to avoid cache */
    std::cout << std::left
              << std::setw(12) << "BlockSize"
              << std::setw(22) << "D2D single-sync"
              << std::setw(22) << "D2D batch-sync"
              << std::setw(10) << "ratio"
              << std::endl;
    std::cout << std::string(66, '-') << std::endl;

    for (int s = 0; s < NUM_SIZES; s++) {
        uint32_t bs = BLOCK_SIZES[s];
        uint64_t totalBytes = (uint64_t)COUNT * bs;

        // warmup
        for (int w = 0; w < WARMUP; w++) {
            CHECK_ACL(aclrtMemcpyAsync(local.va, bs, imported.va, bs, ACL_MEMCPY_DEVICE_TO_DEVICE, stream));
            CHECK_ACL(aclrtSynchronizeStream(stream));
        }

        double t0 = GetTimeUs();
        for (int i = 0; i < COUNT; i++) {
            uint64_t off = (uint64_t)i * bs % (BUF_SIZE - bs);
            CHECK_ACL(aclrtMemcpyAsync((char *)local.va + off, bs, (char *)imported.va + off, bs,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream));
            CHECK_ACL(aclrtSynchronizeStream(stream));
        }
        double singleUs = GetTimeUs() - t0;

        // warmup
        for (int w = 0; w < WARMUP; w++) {
            CHECK_ACL(aclrtMemcpyAsync(local.va, bs, imported.va, bs, ACL_MEMCPY_DEVICE_TO_DEVICE, stream));
        }
        CHECK_ACL(aclrtSynchronizeStream(stream));

        t0 = GetTimeUs();
        for (int i = 0; i < COUNT; i++) {
            uint64_t off = (uint64_t)i * bs % (BUF_SIZE - bs);
            CHECK_ACL(aclrtMemcpyAsync((char *)local.va + off, bs, (char *)imported.va + off, bs,
                                       ACL_MEMCPY_DEVICE_TO_DEVICE, stream));
        }
        CHECK_ACL(aclrtSynchronizeStream(stream));
        double batchUs = GetTimeUs() - t0;

        double ratio = CalcBw(totalBytes, batchUs) / CalcBw(totalBytes, singleUs);
        std::cout << std::left
                  << std::setw(12) << BsStr(bs)
                  << std::setw(22) << BwStr(totalBytes, singleUs)
                  << std::setw(22) << BwStr(totalBytes, batchUs)
                  << std::setw(10) << (std::to_string(ratio).substr(0, 4) + "x")
                  << std::endl;
    }

    /* H2D/D2H table: host <-> devB local VA */
    std::vector<ConcResult> singleResults;
    std::cout << std::endl;
    std::cout << std::left
              << std::setw(12) << "BlockSize"
              << std::setw(22) << "H2D single-sync"
              << std::setw(22) << "H2D batch-sync"
              << std::setw(10) << "ratio"
              << std::setw(22) << "D2H single-sync"
              << std::setw(22) << "D2H batch-sync"
              << std::setw(10) << "ratio"
              << std::endl;
    std::cout << std::string(120, '-') << std::endl;

    for (int s = 0; s < NUM_SIZES; s++) {
        uint32_t bs = BLOCK_SIZES[s];
        uint64_t totalBytes = (uint64_t)COUNT * bs;

        // warmup
        for (int w = 0; w < WARMUP; w++) {
            CHECK_ACL(aclrtMemcpyAsync(local.va, bs, hostMem, bs, ACL_MEMCPY_HOST_TO_DEVICE, stream));
            CHECK_ACL(aclrtSynchronizeStream(stream));
            CHECK_ACL(aclrtMemcpyAsync(hostMem, bs, local.va, bs, ACL_MEMCPY_DEVICE_TO_HOST, stream));
            CHECK_ACL(aclrtSynchronizeStream(stream));
        }

        double t0 = GetTimeUs();
        for (int i = 0; i < COUNT; i++) {
            CHECK_ACL(aclrtMemcpyAsync(local.va, bs, hostMem, bs, ACL_MEMCPY_HOST_TO_DEVICE, stream));
            CHECK_ACL(aclrtSynchronizeStream(stream));
        }
        double h2dSingleUs = GetTimeUs() - t0;

        // warmup
        for (int w = 0; w < WARMUP; w++) {
            CHECK_ACL(aclrtMemcpyAsync(local.va, bs, hostMem, bs, ACL_MEMCPY_HOST_TO_DEVICE, stream));
        }
        CHECK_ACL(aclrtSynchronizeStream(stream));

        t0 = GetTimeUs();
        for (int i = 0; i < COUNT; i++) {
            CHECK_ACL(aclrtMemcpyAsync(local.va, bs, hostMem, bs, ACL_MEMCPY_HOST_TO_DEVICE, stream));
        }
        CHECK_ACL(aclrtSynchronizeStream(stream));
        double h2dBatchUs = GetTimeUs() - t0;

        // warmup
        for (int w = 0; w < WARMUP; w++) {
            CHECK_ACL(aclrtMemcpyAsync(hostMem, bs, local.va, bs, ACL_MEMCPY_DEVICE_TO_HOST, stream));
            CHECK_ACL(aclrtSynchronizeStream(stream));
        }

        t0 = GetTimeUs();
        for (int i = 0; i < COUNT; i++) {
            CHECK_ACL(aclrtMemcpyAsync(hostMem, bs, local.va, bs, ACL_MEMCPY_DEVICE_TO_HOST, stream));
            CHECK_ACL(aclrtSynchronizeStream(stream));
        }
        double d2hSingleUs = GetTimeUs() - t0;

        // warmup
        for (int w = 0; w < WARMUP; w++) {
            CHECK_ACL(aclrtMemcpyAsync(hostMem, bs, local.va, bs, ACL_MEMCPY_DEVICE_TO_HOST, stream));
        }
        CHECK_ACL(aclrtSynchronizeStream(stream));

        t0 = GetTimeUs();
        for (int i = 0; i < COUNT; i++) {
            CHECK_ACL(aclrtMemcpyAsync(hostMem, bs, local.va, bs, ACL_MEMCPY_DEVICE_TO_HOST, stream));
        }
        CHECK_ACL(aclrtSynchronizeStream(stream));
        double d2hBatchUs = GetTimeUs() - t0;

        singleResults.push_back({bs, CalcBw(totalBytes, h2dSingleUs), CalcBw(totalBytes, d2hSingleUs)});

        double h2dRatio = CalcBw(totalBytes, h2dBatchUs) / CalcBw(totalBytes, h2dSingleUs);
        double d2hRatio = CalcBw(totalBytes, d2hBatchUs) / CalcBw(totalBytes, d2hSingleUs);

        std::cout << std::left
                  << std::setw(12) << BsStr(bs)
                  << std::setw(22) << BwStr(totalBytes, h2dSingleUs)
                  << std::setw(22) << BwStr(totalBytes, h2dBatchUs)
                  << std::setw(10) << (std::to_string(h2dRatio).substr(0, 4) + "x")
                  << std::setw(22) << BwStr(totalBytes, d2hSingleUs)
                  << std::setw(22) << BwStr(totalBytes, d2hBatchUs)
                  << std::setw(10) << (std::to_string(d2hRatio).substr(0, 4) + "x")
                  << std::endl;
    }

    MPI_Barrier(MPI_COMM_WORLD);

    std::vector<ConcResult> concResults1;
    RunConcTest(local.va, hostMem, stream, concResults1);

    MPI_Barrier(MPI_COMM_WORLD);

    std::vector<ConcResult> concResults0(NUM_SIZES);
    MPI_Recv(concResults0.data(), NUM_SIZES * sizeof(ConcResult), MPI_BYTE, 0, 200,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    PrintConcComparison(concResults0, concResults1, singleResults, devA, devB);

    MPI_Barrier(MPI_COMM_WORLD);

    CHECK_ACL(aclrtDestroyStream(stream));
    free(hostMem);
    FreeVmmP2P(local);
    FreeVmmP2P(imported);

    CHECK_ACL(aclrtResetDevice(devB));
    CHECK_ACL(aclFinalize());
    MPI_Finalize();
    return 0;
}
