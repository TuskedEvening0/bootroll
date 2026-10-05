#pragma once
// Stateless sysfs helpers shared by the Linux platform layer. All functions
// are pure file reads - safe to call from any thread, no device nodes opened.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bootroll {
namespace sysfs {

// Read a whole (small) file and trim trailing whitespace; "" on error.
std::string readFileTrimmed(const std::string& path);

bool fileExists(const std::string& path);

// /sys/block entries after filtering virtual/ephemeral devices
// (loop*/ram*/zram*/rom*/fd*/sr* and md*/dm-* RAIDs, per M8_PLAN §3),
// sorted lexicographically for a deterministic discovery order.
std::vector<std::string> scanBlockDevNames();

// "/dev/<name>" for a /sys/block entry name.
std::string devNodePath(const std::string& blockName);

// Best-effort model string (device/model, device/name fallback; trailing
// spaces trimmed; "" when the device has none).
std::string modelOfBlockDev(const std::string& blockName);

// Project bus type strings: "SATA"/"USB"/"NVMe"/"SCSI"/"SD"/"MMC"/"Virtual"/"ATA"/"Unknown".
std::string busTypeOfBlockDev(const std::string& blockName);

// /sys/block/<name>/size is in 512-byte sectors (always, regardless of the
// logical block size). 0 on error.
uint64_t diskSizeBytes(const std::string& blockName);

// /sys/block/<name>/queue/logical_block_size, defaulting to 512.
uint32_t logicalBlockSize(const std::string& blockName);

// Partition directories below /sys/block/<name> (entries with a "partition"
// attribute), keyed by their kernel partition number -> dir name.
std::map<uint32_t, std::string> partitionDirsOfDisk(const std::string& blockName);

// major:minor string ("259:4") of a sysfs block/patition dir; "" on error.
std::string devNumberOfDir(const std::string& sysfsDir);

// Directory name for /sys/dev/block/<major>:<minor> ("nvme0n1p1"), "" on error.
std::string blockDirNameOfDevNumber(const std::string& majorMinor);

} // namespace sysfs
} // namespace bootroll
