#pragma once
// Linux volume enumeration (/proc/self/mountinfo + sysfs), counterpart of
// platform/win/VolumeWin.cpp. Pure file reads: safe on any thread, no device
// nodes are opened here (a sick device cannot stall the caller).
#include <string>
#include <vector>

#include "core/disk/DiskInfo.h"
#include "platform/IPlatform.h"

namespace bootroll {

// One line of /proc/self/mountinfo (deduplicated upstream).
struct MountEntry {
    std::string devNumber;    // "major:minor", e.g. "259:4"
    std::string mountPoint;   // decoded mount path
    std::string fsType;       // "ext4", "vfat", "ntfs3", ...
};

// Parse /proc/self/mountinfo. One entry per device (first mount point wins,
// bind mounts collapse onto the first).
std::vector<MountEntry> parseMountInfo();

// Full volume list: mount points as driveLetter, fs type (uppercased) as
// fsName, statvfs sizes, label best-effort via /dev/disk/by-label, and
// diskNumber/partitionNumber/isEsp best-effort via sysfs + udev db.
std::vector<VolumeInfo> enumerateVolumesLinux();

// Fill PartitionInfo driveLetter/label/fsName from the mount table (used by
// DiskAccessLinux::fillDiskDetails, mirrors DiskWin's attachVolumeData).
void attachVolumeDataLinux(DiskInfo& disk);

// Device node path for a "major:minor" pair ("/dev/nvme0n1p1"), "" if unknown.
// Resolves through /sys/dev/block (no device access).
std::string devNodeForDevNumber(const std::string& majorMinor);

} // namespace bootroll
