#pragma once
// Partition filesystem identification from raw boot-sector bytes, with no OS
// involvement (the blkid-style magic checks bootroll actually cares about:
// the FAT family / NTFS / exFAT for the PBR screens, plus the common Linux
// filesystems for display). Windows resolves filesystems through the OS
// (GetVolumeInformationW); Linux probes this for partitions that are not
// mounted and therefore absent from /proc/self/mountinfo.
#include <cstddef>
#include <cstdint>
#include <string>

namespace bootroll {

// Identify a partition's filesystem from its boot sector (the first bytes of
// the partition, at least 512; the ext family needs 0x44C). Returns "NTFS",
// "EXFAT", "FAT32", "FAT16", "FAT12", "EXT4", "EXT3", "EXT2" or "XFS"; ""
// when nothing matches (blank, unknown or unsupported). Names follow the
// Windows fsName convention that PartitionInfo::fsName carries (upper case,
// PbrScreen::kindForFsName-ready).
std::string probeFilesystemName(const uint8_t* bootSector, size_t size);

// True when the buffer holds the btrfs superblock magic ("_BHRfS_M", found at
// partition offset 0x10040). Kept separate from probeFilesystemName because
// the magic lives far beyond the boot sector.
bool isBtrfsSuperblock(const uint8_t* p, size_t size);

} // namespace bootroll
