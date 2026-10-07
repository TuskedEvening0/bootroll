#include "core/disk/DiskInfo.h"
#include <cctype>
#include <cstdio>

namespace bootroll {

std::string formatSize(uint64_t bytes)
{
    const char* units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    char buf[48];
    if (u == 0)
        std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(bytes), units[u]);
    else if (v >= 100.0)
        std::snprintf(buf, sizeof(buf), "%.0f %s", v, units[u]);
    else if (v >= 10.0)
        std::snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    else
        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
    return buf;
}

// GPT type GUID of an EFI System Partition (lowercase canonical form).
constexpr const char* kEspTypeGuid = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";

bool isEspTypeGuid(const std::string& gptTypeGuid)
{
    std::string g = gptTypeGuid;
    for (char& c : g) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return g == kEspTypeGuid;
}

std::string diskShortName(const DiskInfo& d)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Disk %u", d.number);
    return buf;
}

} // namespace bootroll
