#pragma once
// Partition boot record (volume boot sector) helpers: filesystem detection
// from the BPB/OEM area and BPB geometry patching used when installing a PBR
// taken from a reference volume onto a different partition.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bootroll {

enum class PbrKind {
    NotPbr,   // no 0x55AA signature
    Unknown,
    Ntfs,
    Fat12,
    Fat16,
    Fat32,
    ExFat,
    Grub4dos, // grldr-installed PBR
};

std::string_view pbrKindLabel(PbrKind k);

// Best-effort filesystem detection from a raw 512-byte volume sector.
PbrKind detectPbrKind(const uint8_t* sector, size_t size);

// --- BPB geometry patching (fields the partition table provides) ------------
// All functions require a >= 512-byte sector and only touch geometry fields;
// loader code and filesystem layout fields are left untouched.

// NTFS VBR: hidden sectors (QWORD @0x1C) + volume total sectors (QWORD @0x28).
// Returns false when the sector does not look like an NTFS VBR.
bool patchNtfsBpb(uint8_t* sector, size_t size, uint64_t hiddenSectors,
                  uint64_t totalSectors);

// FAT BPB: hidden sectors (DWORD @0x1C) + total32 (DWORD @0x20); total16
// (@0x13) is set when the volume fits, else zeroed per FAT convention.
// sectorsPerCluster stays as-is (it belongs to the filesystem format).
bool patchFatBpb(uint8_t* sector, size_t size, uint32_t hiddenSectors,
                 uint32_t totalSectors);

// exFAT VBR: volume offset (QWORD @0x40) + volume length in sectors (QWORD @0x48).
bool patchExFatBpb(uint8_t* sector, size_t size, uint64_t hiddenSectors,
                   uint64_t totalSectors);

} // namespace bootroll
