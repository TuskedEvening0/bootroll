#pragma once
// Portable local-time conversion: MSVC localtime_s vs POSIX localtime_r.
// CRT-level portability only (standard <ctime>, no OS API).
#include <ctime>

namespace bootroll {

inline std::tm localTm(std::time_t t)
{
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

} // namespace bootroll
