#ifndef HYBM_SDMA_SERVICE_OPTIONS_H
#define HYBM_SDMA_SERVICE_OPTIONS_H

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <string>
#include "hybm_sdma_contract.h"

namespace ock {
namespace mf {
struct SdmaServiceOptions {
    uint32_t epochSeconds = HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT;
    uint32_t leaseMs = 10000;
    uint32_t stopMs = 5000;
};

inline bool ParseSdmaUnsigned(const std::string &value, uint32_t minimum, uint32_t &out)
{
    if (value.empty())
        return true;
    if (value.front() == '-' || value.front() == '+' || value.front() < '0' || value.front() > '9')
        return false;
    errno = 0;
    char *end = nullptr;
    unsigned long long n = std::strtoull(value.c_str(), &end, 10);
    if (errno || *end || n < minimum || n > std::numeric_limits<uint32_t>::max())
        return false;
    out = static_cast<uint32_t>(n);
    return true;
}

// mode为空或resident时常驻；正整数表示有限轮次的秒数。
inline bool ResolveSdmaOptions(const std::string &mode, const std::string &lease, const std::string &stop,
                               SdmaServiceOptions &out)
{
    out = SdmaServiceOptions{};
    if (!ParseSdmaUnsigned(lease, 3000, out.leaseMs) || !ParseSdmaUnsigned(stop, 1000, out.stopMs) ||
        out.stopMs > static_cast<uint32_t>(std::numeric_limits<int32_t>::max() - 1000))
        return false;
    if (mode.empty() || mode == "resident")
        return true;
    return ParseSdmaUnsigned(mode, 1, out.epochSeconds) && out.epochSeconds != HYBM_SDMA_ORCH_EPOCH_BUDGET_RESIDENT;
}
} // namespace mf
} // namespace ock
#endif
