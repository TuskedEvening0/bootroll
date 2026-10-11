#include "core/disk/FileSystemProbe.h"

#include <cstdint>
#include <cstring>

namespace bootroll {

namespace {

uint16_t rd16(const uint8_t* p)
{
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t rd32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

bool isPow2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

bool hasSignature(const uint8_t* sector)
{
    return sector[0x1FE] == 0x55 && sector[0x1FF] == 0xAA;
}

// FAT-family classification from a validated BPB. Mirrors the parse and the
// FAT32-vs-FAT12/16 rules of core/fat/FatVolume::open (rootEntries and
// fatSize16 must both be 0 for FAT32 - real ESPs are often smaller than the
// spec-recommended cluster count, so the cluster count alone misjudges).
bool fatTypeFromBpb(const uint8_t* b, std::string* out)
{
    const uint16_t bytesPerSector = rd16(b + 11);
    const uint8_t sectorsPerCluster = b[13];
    const uint16_t reserved = rd16(b + 14);
    const uint8_t fatCount = b[16];
    const uint16_t rootEntries = rd16(b + 17);
    const uint16_t total16 = rd16(b + 19);
    const uint16_t fatSize16 = rd16(b + 22);
    const uint32_t total32 = rd32(b + 32);
    const uint32_t fatSize32 = rd32(b + 36);
    const uint32_t rootCluster = rd32(b + 44);

    // Sanity: only self-consistent BPBs count (same thresholds as FatVolume).
    if (!(bytesPerSector >= 512 && bytesPerSector <= 4096 && isPow2(bytesPerSector)) ||
        !isPow2(sectorsPerCluster) || sectorsPerCluster > 128 ||
        reserved == 0 || fatCount == 0 || fatCount > 8) {
        return false;
    }
    const uint32_t totalSectors = total16 != 0 ? total16 : total32;
    const uint32_t fatSizeSectors = fatSize16 != 0 ? fatSize16 : fatSize32;
    if (totalSectors == 0 || fatSizeSectors == 0) {
        return false;
    }

    if (rootEntries == 0 && fatSize16 == 0) {
        if (rootCluster < 2) {
            return false;
        }
        *out = "FAT32";
        return true;
    }
    const uint32_t rootDirSectors =
        uint32_t((uint64_t(rootEntries) * 32 + bytesPerSector - 1) / bytesPerSector);
    const uint64_t dataSectors = totalSectors -
        (uint64_t(reserved) + uint64_t(fatCount) * fatSizeSectors + rootDirSectors);
    const uint32_t clusters = uint32_t(dataSectors / sectorsPerCluster);
    if (clusters < 2) {
        return false;
    }
    *out = clusters <= 4085 ? "FAT12" : "FAT16";
    return true;
}

// ext2/3/4 share the 0xEF53 magic at superblock offset 56 (byte 0x438 of the
// partition - note the superblock starts at 1024, so this needs more than one
// 512-byte sector); the family member comes from the feature flags that
// follow. `b` must span at least 0x44C bytes.
std::string extTypeFromSuperblock(const uint8_t* b)
{
    const uint32_t revLevel = rd32(b + 0x43C);
    if (revLevel == 0) {
        return "EXT2"; // revision 0 predates the feature flags
    }
    const uint32_t featureCompat = rd32(b + 0x440);
    const uint32_t featureIncompat = rd32(b + 0x444);
    const uint32_t featureRoCompat = rd32(b + 0x448);
    const bool hasExtents = (featureIncompat & 0x40) != 0;     // EXT4_EXTENTS_FL
    const bool has64Bit = (featureRoCompat & 0x80) != 0;       // 64BIT
    const bool hasMetadataCsum = (featureRoCompat & 0x200) != 0;
    if (hasExtents || has64Bit || hasMetadataCsum) {
        return "EXT4";
    }
    if ((featureCompat & 0x4) != 0) {
        return "EXT3"; // HAS_JOURNAL
    }
    return "EXT2";
}

} // namespace

std::string probeFilesystemName(const uint8_t* bootSector, size_t size)
{
    if (bootSector == nullptr || size < 512) {
        return {};
    }

    // XFS: "XFSB" magic at offset 0 (checked before the BPB heuristics so an
    // accidentally BPB-like superblock cannot win).
    if (std::memcmp(bootSector, "XFSB", 4) == 0) {
        return "XFS";
    }

    if (hasSignature(bootSector)) {
        // OEM name at offset 3, the same anchor detectPbrKind uses.
        if (std::memcmp(bootSector + 0x03, "NTFS    ", 8) == 0) {
            return "NTFS";
        }
        if (std::memcmp(bootSector + 0x03, "EXFAT   ", 8) == 0) {
            return "EXFAT";
        }
        // FAT family: jump instruction + a BPB that parses self-consistently.
        if ((bootSector[0] == 0xEB || bootSector[0] == 0xE9)) {
            std::string fatType;
            if (fatTypeFromBpb(bootSector, &fatType)) {
                return fatType;
            }
        }
        // ext2/3/4 magic (0x2EF5 little-endian at 0x438). The superblock
        // starts at byte 1024 of the partition, so this check needs 0x44C
        // bytes - callers reading only a single 512-byte sector (too short
        // for the feature flags anyway) must not touch it.
        if (size >= 0x44C && rd16(bootSector + 0x438) == 0xEF53) {
            return extTypeFromSuperblock(bootSector);
        }
    }
    return {};
}

bool isBtrfsSuperblock(const uint8_t* p, size_t size)
{
    return p != nullptr && size >= 8 && std::memcmp(p, "_BHRfS_M", 8) == 0;
}

} // namespace bootroll
