// Unit tests for the boot-sector filesystem probe (core/disk/FileSystemProbe).
// The Linux platform layer uses it to fill PartitionInfo::fsName for
// partitions that are not mounted (they are invisible to mountinfo), so every
// magic family here needs a fixture built the way mkfs writes it.
#include <doctest.h>

#include "core/disk/FileSystemProbe.h"

#include <cstring>
#include <string>
#include <vector>

using namespace bootroll;

namespace {

std::vector<uint8_t> blankSector()
{
    std::vector<uint8_t> b(512, 0);
    b[0x1FE] = 0x55;
    b[0x1FF] = 0xAA;
    return b;
}

void putAscii(std::vector<uint8_t>& b, size_t offset, const char* s)
{
    std::memcpy(b.data() + offset, s, std::strlen(s));
}

void putLe16(std::vector<uint8_t>& b, size_t offset, uint16_t v)
{
    b[offset] = uint8_t(v);
    b[offset + 1] = uint8_t(v >> 8);
}

void putLe32(std::vector<uint8_t>& b, size_t offset, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b[offset + i] = uint8_t(v >> (8 * i));
    }
}

// FAT BPB with the given geometry (mkdosfs-style fields only; the caller
// chooses FAT32 by zeroing rootEntries/fatSize16).
std::vector<uint8_t> fatSector(uint16_t total16, uint32_t total32,
                               uint16_t fatSize16, uint32_t fatSize32,
                               uint16_t rootEntries, uint8_t sectorsPerCluster,
                               uint8_t jump)
{
    std::vector<uint8_t> b = blankSector();
    b[0] = jump;
    putLe16(b, 11, 512);              // bytes per sector
    b[13] = sectorsPerCluster;
    putLe16(b, 14, 32);               // reserved sectors
    b[16] = 2;                        // FAT count
    putLe16(b, 17, rootEntries);
    putLe16(b, 19, total16);
    putLe16(b, 22, fatSize16);
    putLe32(b, 32, total32);
    putLe32(b, 36, fatSize32);
    putLe32(b, 44, 2);                // root cluster (FAT32)
    return b;
}

// ext superblock lives at byte 1024 of the partition, so the fixture buffer
// must span 0x44C bytes (probeFilesystemName refuses to look at the magic on
// shorter buffers).
std::vector<uint8_t> extSector(uint32_t revLevel, uint32_t featureCompat,
                               uint32_t featureIncompat,
                               uint32_t featureRoCompat)
{
    std::vector<uint8_t> b(0x44C, 0);
    b[0x1FE] = 0x55;
    b[0x1FF] = 0xAA;
    putLe16(b, 0x438, 0xEF53);        // s_magic
    putLe32(b, 0x43C, revLevel);      // s_rev_level
    putLe32(b, 0x440, featureCompat); // s_feature_compat
    putLe32(b, 0x444, featureIncompat);
    putLe32(b, 0x448, featureRoCompat);
    return b;
}

} // namespace

TEST_CASE("probeFilesystemName: NTFS and exFAT by OEM name")
{
    SUBCASE("NTFS") {
        std::vector<uint8_t> b = blankSector();
        putAscii(b, 0x03, "NTFS    ");
        CHECK(probeFilesystemName(b.data(), b.size()) == "NTFS");
    }
    SUBCASE("exFAT") {
        std::vector<uint8_t> b = blankSector();
        putAscii(b, 0x03, "EXFAT   ");
        CHECK(probeFilesystemName(b.data(), b.size()) == "EXFAT");
    }
    SUBCASE("missing 55AA -> unknown (no blind OEM trust)") {
        std::vector<uint8_t> b(512, 0);
        putAscii(b, 0x03, "NTFS    ");
        CHECK(probeFilesystemName(b.data(), b.size()).empty());
    }
}

TEST_CASE("probeFilesystemName: FAT family split by BPB shape")
{
    SUBCASE("FAT32 (rootEntries and fatSize16 both zero)") {
        std::vector<uint8_t> b = fatSector(0, 2097152, 0, 2048, 0, 8, 0xEB);
        CHECK(probeFilesystemName(b.data(), b.size()) == "FAT32");
    }
    SUBCASE("FAT16 (large cluster count, E9 jump)") {
        std::vector<uint8_t> b = fatSector(62000, 0, 128, 0, 512, 4, 0xE9);
        CHECK(probeFilesystemName(b.data(), b.size()) == "FAT16");
    }
    SUBCASE("FAT12 (small cluster count)") {
        std::vector<uint8_t> b = fatSector(8000, 0, 32, 0, 512, 2, 0xEB);
        CHECK(probeFilesystemName(b.data(), b.size()) == "FAT12");
    }
    SUBCASE("invalid BPB (reserved=0) -> unknown") {
        std::vector<uint8_t> b = fatSector(62000, 0, 128, 0, 512, 4, 0xEB);
        b[14] = 0;
        CHECK(probeFilesystemName(b.data(), b.size()).empty());
    }
}

TEST_CASE("probeFilesystemName: ext family split by feature flags")
{
    SUBCASE("revision 0 -> EXT2") {
        std::vector<uint8_t> b = extSector(0, 0, 0, 0);
        CHECK(probeFilesystemName(b.data(), b.size()) == "EXT2");
    }
    SUBCASE("revision 1, no features -> EXT2") {
        std::vector<uint8_t> b = extSector(1, 0, 0, 0);
        CHECK(probeFilesystemName(b.data(), b.size()) == "EXT2");
    }
    SUBCASE("has_journal only -> EXT3") {
        std::vector<uint8_t> b = extSector(1, 0x4, 0, 0);
        CHECK(probeFilesystemName(b.data(), b.size()) == "EXT3");
    }
    SUBCASE("extents -> EXT4") {
        std::vector<uint8_t> b = extSector(1, 0x4, 0x40, 0);
        CHECK(probeFilesystemName(b.data(), b.size()) == "EXT4");
    }
    SUBCASE("64bit -> EXT4") {
        std::vector<uint8_t> b = extSector(1, 0, 0, 0x80);
        CHECK(probeFilesystemName(b.data(), b.size()) == "EXT4");
    }
    SUBCASE("a single 512-byte sector cannot reach the magic -> unknown") {
        // The superblock sits at byte 1024; the probe must not peek past a
        // one-sector buffer to find it.
        std::vector<uint8_t> b = extSector(1, 0x4, 0x40, 0);
        REQUIRE(b.size() >= 512);
        CHECK(probeFilesystemName(b.data(), 512).empty());
        CHECK(probeFilesystemName(b.data(), b.size()) == "EXT4");
    }
}

TEST_CASE("probeFilesystemName: XFS wins before the BPB heuristics")
{
    std::vector<uint8_t> b = blankSector();
    putAscii(b, 0, "XFSB");
    CHECK(probeFilesystemName(b.data(), b.size()) == "XFS");
}

TEST_CASE("probeFilesystemName: unrecognized and degenerate inputs")
{
    SUBCASE("garbage with signature -> unknown") {
        std::vector<uint8_t> b = blankSector();
        for (size_t i = 0; i < 0x100; ++i) {
            b[i] = uint8_t(0xA0 + i);
        }
        CHECK(probeFilesystemName(b.data(), b.size()).empty());
    }
    SUBCASE("short buffer -> unknown") {
        std::vector<uint8_t> b(64, 0);
        CHECK(probeFilesystemName(b.data(), b.size()).empty());
    }
    SUBCASE("null buffer -> unknown") {
        CHECK(probeFilesystemName(nullptr, 512).empty());
    }
}

TEST_CASE("isBtrfsSuperblock: magic at partition offset 0x10040")
{
    std::vector<uint8_t> b(8, 0);
    CHECK_FALSE(isBtrfsSuperblock(b.data(), b.size()));
    putAscii(b, 0, "_BHRfS_M");
    CHECK(isBtrfsSuperblock(b.data(), b.size()));
    CHECK_FALSE(isBtrfsSuperblock(nullptr, 8));
}
