#include "hybm_physical_device_id.h"
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <limits>
using ock::mf::HybmPhysicalDeviceIdOf;
static uint32_t map(const char *s, unsigned id) {
    if (s == nullptr) { unsetenv("ASCEND_RT_VISIBLE_DEVICES"); }
    else { setenv("ASCEND_RT_VISIBLE_DEVICES", s, 1); }
    return HybmPhysicalDeviceIdOf(id);
}
int main() {
    constexpr auto BAD = std::numeric_limits<uint32_t>::max();
    assert(map(nullptr, 0) == 0);
    assert(map(nullptr, 7) == 7);
    assert(map("0,1", 0) == 0);
    assert(map("0,1", 1) == 1);
    assert(map("2,3", 0) == 2);
    assert(map("2,3", 1) == 3);
    assert(map("4,5", 1) == 5);
    assert(map("6,7", 0) == 6);
    assert(map("6,7", 1) == 7);
    assert(map("2,3", 2) == BAD);
    assert(map("", 0) == BAD);
    assert(map("2,,3", 0) == BAD);
    assert(map("2,", 0) == BAD);
    assert(map(",2", 0) == BAD);
    assert(map("2, 3", 0) == BAD);
    assert(map("2,3 ", 0) == BAD);
    assert(map("2;3", 0) == BAD);
    assert(map("x,3", 0) == BAD);
    assert(map("2,2", 0) == BAD);
    assert(map("4294967295,3", 0) == BAD);
    assert(map("4294967294,3", 0) == 4294967294U);
    assert(map("42949672950,3", 0) == BAD);
    assert(map("1,2,3", BAD) == BAD);
    return 0;
}
