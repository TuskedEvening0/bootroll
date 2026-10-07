#pragma once
// Portable disk/partition data model (no OS headers allowed in core/).
#include <cstdint>
#include <string>
#include <vector>

namespace bootroll {

enum class PartitionStyle {
    Unknown,
    Mbr,
    Gpt,
};

struct PartitionInfo {
    uint32_t number = 0;        // 1-based partition number on its disk
    PartitionStyle style = PartitionStyle::Unknown;
    uint64_t offsetBytes = 0;
    uint64_t sizeBytes = 0;

    // MBR
    uint8_t typeCode = 0;       // MBR partition type byte (0x07, 0x0C, 0x17, ...)
    bool bootIndicator = false; // active/bootable flag
    bool hidden = false;        // derived from hidden FAT/NTFS type codes

    // GPT
    std::string gptTypeGuid;    // lowercase guid string, e.g. "c12a7328-f81f-..."
    std::string gptPartGuid;    // lowercase unique partition guid ("" on MBR)
    std::string gptName;        // UTF-8 partition name

    // Mounted volume info (optional; empty when not mounted)
    std::string driveLetter;    // "C:" or ""
    std::string label;
    std::string fsName;         // "NTFS", "FAT32", ... ("" when unmountable, e.g. ESP)
};

struct DiskInfo {
    uint32_t number = 0;        // OS disk index (Windows: PhysicalDriveN)
    uint64_t totalBytes = 0;
    uint32_t sectorSize = 512;
    std::string model;          // best-effort product/vendor string (UTF-8)
    std::string busType;        // "SATA", "USB", "NVMe", "Virtual", ...
    bool isVhd = false;         // best-effort virtual disk detection
    PartitionStyle style = PartitionStyle::Unknown;
    std::vector<PartitionInfo> partitions;
};

// Human-readable size: 512 GB, 1.5 TB, ...
std::string formatSize(uint64_t bytes);

// "Disk 0" style short name without model.
std::string diskShortName(const DiskInfo& d);

// True when the partition's GPT type GUID is the EFI System Partition
// (case-insensitive). Single source for the ESP check (UefiScreen,
// EspFileDialog and the M11 loader scan share it).
bool isEspTypeGuid(const std::string& gptTypeGuid);

} // namespace bootroll
