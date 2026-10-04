#pragma once
// Win32 volume enumeration helpers (used by DiskWin and PlatformWin).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "platform/IPlatform.h"

namespace bootroll {

// Enumerate all mounted volumes (letters, labels, FS, backing disk/partition,
// ESP detection). Purely read-only; works without elevation.
std::vector<VolumeInfo> enumerateVolumesWin();

} // namespace bootroll
