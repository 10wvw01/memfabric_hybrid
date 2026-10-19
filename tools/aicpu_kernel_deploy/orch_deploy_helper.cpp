/*
 * orch_deploy_helper — CP1 orchestrator kernel deployment helper (fixed commands).
 *
 * Why this exists (2026-10-18): under torch_npu, ONE KFC system() command
 * with a nonzero exit code (e.g. the expected `test -f` probe-miss on a
 * fresh device) permanently faults the whole device context of the calling
 * process — every later launch, including ordinary torch ops, fails with
 * 507018/aicpu-exception (measured: neither time, a fresh stream, nor
 * anything else in-process recovers it). A plain ACL process is unaffected.
 * Therefore the probe + auto-deploy chain must run in a fresh process:
 * the library fork+execs this helper and never executes a KFC command in
 * its own context.
 *
 * The helper runs only FIXED commands with controlled paths (same security
 * posture as the in-library deploy chain; this is NOT a general shell tool
 * and must not be extended to accept arbitrary commands):
 *   1. probe    test -f <dst>                    (exit 1 = not deployed yet)
 *   2. clean    rm -f /home/CustAiCpuUser/lib/libmf_sdma_orch.so.*
 *   3. migrate  aclrtBinaryLoadFromFile(cust json, CPU mode=1) + GetFunction
 *   4. copy     cat <staged> > <dst> && chmod 644 <dst>
 *   5. verify   grep -q HybmSdmaOrchEpoch <dst> and exact size match
 *
 * Usage: orch_deploy_helper <devId> <launchJsonPath>
 * Exit codes: 0 = deployed or already present; 3 = probe says present;
 * 1 = usage/ACL failure; 2 = deploy/verify failed.
 */
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include "acl/acl.h"
#include "acl/acl_rt.h"

static constexpr const char *DEV_SEARCH_DIR = "/usr/lib64/aicpu_kernels/0/aicpu_kernels_device";
static constexpr const char *CUST_DEV_DIR = "/home/CustAiCpuUser/lib";
static constexpr const char *CUST_SO = "libmf_sdma_orch.so";
static constexpr const char *CUST_JSON = "libmf_sdma_orch.json";
static constexpr const char *FUNC_NAME = "HybmSdmaOrchEpoch";
static constexpr const char *DEPLOY_LOCK = "/tmp/.mf_sdma_orch_deploy.lock";
static constexpr uint32_t KFC_TIMEOUT_S = 25;

/* Same host-side deploy mutex as the library: two ranks' helpers must not
 * migrate/cat concurrently (glob concatenation hazard). */
class FlockGuard {
public:
    FlockGuard() : fd_(open(DEPLOY_LOCK, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600))
    {
        if (fd_ >= 0) {
            (void)flock(fd_, LOCK_EX);
        }
    }
    ~FlockGuard()
    {
        if (fd_ >= 0) {
            (void)flock(fd_, LOCK_UN);
            (void)close(fd_);
        }
    }
private:
    int fd_ = -1;
};

static aclrtBinHandle g_bh = nullptr;
static aclrtFuncHandle g_fh = nullptr;
static aclrtStream g_s = nullptr;

static int kfc_exec(const std::string &cmd)
{
    char buf[512] = {0};
    strncpy(buf, cmd.c_str(), sizeof(buf) - 1);
    aclrtLaunchKernelAttr attr = {};
    attr.id = aclrtLaunchKernelAttrId::ACL_RT_LAUNCH_KERNEL_ATTR_TIMEOUT;
    attr.value.timeout = KFC_TIMEOUT_S;
    aclrtLaunchKernelCfg cfg = {};
    cfg.attrs = &attr;
    cfg.numAttrs = 1;
    aclError r = aclrtLaunchKernelWithHostArgs(g_fh, 1, g_s, &cfg, buf, strlen(buf) + 1, nullptr, 0);
    if (r != 0) {
        printf("[helper] kfc launch failed ret=%d cmd='%s'\n", (int)r, cmd.c_str());
        return (int)r;
    }
    r = aclrtSynchronizeStream(g_s);
    if (r != 0) {
        printf("[helper] kfc sync=%d cmd='%s'\n", (int)r, cmd.c_str());
        return (int)r;
    }
    return 0;
}

static int kfc_init()
{
    char path[64] = {0};
    int fd = -1;
    for (unsigned i = 0; i < 16 && fd < 0; i++) {
        snprintf(path, sizeof(path), "/tmp/mf_kfc_helper_%d_%u.json", (int)getpid(), i);
        fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) {
        return -1;
    }
    static constexpr char KFC_JSON[] =
        "{\"K\":{\"opInfo\":{\"opKernelLib\":\"KFCKernel\",\"kernelSo\":\"kfc_min.so\","
        "\"functionName\":\"system\"}}}";
    const ssize_t want = (ssize_t)strlen(KFC_JSON);
    if (write(fd, KFC_JSON, want) != want) {
        close(fd);
        unlink(path);
        return -1;
    }
    close(fd);
    aclrtBinaryLoadOption opt = {};
    opt.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    opt.value.cpuKernelMode = 0;
    aclrtBinaryLoadOptions opts = {};
    opts.options = &opt;
    opts.numOpt = 1;
    int rc = (int)aclrtBinaryLoadFromFile(path, &opts, &g_bh);
    unlink(path);
    if (rc != 0) {
        printf("[helper] kfc load failed ret=%d\n", rc);
        return rc;
    }
    rc = (int)aclrtBinaryGetFunction(g_bh, "K", &g_fh);
    if (rc != 0) {
        printf("[helper] kfc getfunc failed ret=%d\n", rc);
        return rc;
    }
    rc = (int)aclrtCreateStream(&g_s);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <devId> <launchJsonPath>\n", argv[0]);
        return 1;
    }
    const int dev = atoi(argv[1]);
    const std::string launchJson = argv[2];
    auto slash = launchJson.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".") : launchJson.substr(0, slash);
    const auto soSlash = launchJson.find_last_of('/');
    std::string soName = (soSlash == std::string::npos) ? launchJson : launchJson.substr(soSlash + 1);
    if (soName.size() > 5 && soName.compare(soName.size() - 5, 5, ".json") == 0) {
        soName.resize(soName.size() - 5);
    }
    soName += ".so";
    const std::string dst = std::string(DEV_SEARCH_DIR) + "/" + soName;

    if (aclInit(nullptr) != 0 || aclrtSetDevice(dev) != 0) {
        printf("[helper] acl init failed\n");
        return 1;
    }
    if (kfc_init() != 0) {
        printf("[helper] kfc channel init failed\n");
        return 1;
    }
    FlockGuard lock;
    // 1. probe (nonzero exit is expected here and harmless: fresh process)
    if (kfc_exec("test -f " + dst) == 0) {
        printf("[helper] kernel already present: %s\n", dst.c_str());
        return 3;
    }
    printf("[helper] kernel missing, deploying: %s\n", dst.c_str());
    // 2. clean staged residue
    if (kfc_exec(std::string("rm -f ") + CUST_DEV_DIR + "/" + CUST_SO + ".*") != 0) {
        printf("[helper] clean residue failed\n");
        return 2;
    }
    // 3. migrate via CUST channel (GetFunction triggers the device write)
    struct stat st = {};
    const std::string hostSo = dir + "/" + CUST_SO;
    if (stat(hostSo.c_str(), &st) != 0) {
        printf("[helper] host kernel so missing: %s\n", hostSo.c_str());
        return 2;
    }
    const uint64_t soSize = (uint64_t)st.st_size;
    aclrtBinaryLoadOption opt = {};
    opt.type = aclrtBinaryLoadOptionType::ACL_RT_BINARY_LOAD_OPT_CPU_KERNEL_MODE;
    opt.value.cpuKernelMode = 1;
    aclrtBinaryLoadOptions opts = {};
    opts.options = &opt;
    opts.numOpt = 1;
    aclrtBinHandle cbh = nullptr;
    const std::string custJson = dir + "/" + CUST_JSON;
    int rc = (int)aclrtBinaryLoadFromFile(custJson.c_str(), &opts, &cbh);
    if (rc != 0) {
        printf("[helper] cust load failed ret=%d json=%s\n", rc, custJson.c_str());
        return 2;
    }
    aclrtFuncHandle cfh = nullptr;
    rc = (int)aclrtBinaryGetFunction(cbh, FUNC_NAME, &cfh);
    if (rc != 0) {
        printf("[helper] cust getfunc ret=%d (tolerated, verify later)\n", rc);
    }
    (void)cbh; /* keep the handle alive: this process exits only after verify */
    // 4. copy staged -> search path
    const std::string src = std::string(CUST_DEV_DIR) + "/" + CUST_SO + ".*";
    if (kfc_exec("cat " + src + " > " + dst + " && chmod 644 " + dst) != 0) {
        printf("[helper] copy failed\n");
        return 2;
    }
    // 5. verify: entry symbol present + exact size
    const std::string verify = "grep -q " + std::string(FUNC_NAME) + " " + dst +
        " && ls -l " + dst + " | awk '{print $5}' | grep -qx " + std::to_string(soSize);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (kfc_exec(verify) == 0) {
            printf("[helper] deployed and verified: %s size=%llu\n",
                   dst.c_str(), (unsigned long long)soSize);
            return 0;
        }
        sleep(1); /* staged file writes are asynchronous; retry is idempotent */
    }
    printf("[helper] verify failed after retries\n");
    return 2;
}
