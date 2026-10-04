// Unit tests for the read-only FAT12/16/32 volume reader (M7 ESP browser).
// Builds in-memory FAT images - no OS access, no real disks.
#include "core/fat/FatVolume.h"

#include "doctest.h"

#include <cstring>
#include <string>
#include <vector>

using namespace bootroll;

namespace {

constexpr uint32_t kSec = 512;

void put16(std::vector<uint8_t>& v, size_t off, uint16_t x)
{
    v[off] = uint8_t(x & 0xFF);
    v[off + 1] = uint8_t(x >> 8);
}

void put32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
    v[off] = uint8_t(x & 0xFF);
    v[off + 1] = uint8_t((x >> 8) & 0xFF);
    v[off + 2] = uint8_t((x >> 16) & 0xFF);
    v[off + 3] = uint8_t((x >> 24) & 0xFF);
}

// LFN checksum over the 11-byte 8.3 short name (same as FatVolume).
uint8_t lfnChecksumOf(const char* name11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; ++i) {
        sum = uint8_t(((sum >> 1) | ((sum & 1) << 7)) + uint8_t(name11[i]));
    }
    return sum;
}

// 32-byte short directory entry at a raw byte offset.
void shortEntry(std::vector<uint8_t>& img, size_t off, const char name11[11],
                uint8_t attr, uint32_t cluster, uint32_t size, uint8_t ntFlags = 0,
                uint16_t wdate = 0x5B34, uint16_t wtime = 0x645C)
{
    std::memset(img.data() + off, 0, 32);
    std::memcpy(img.data() + off, name11, 11);
    img[off + 11] = attr;
    img[off + 12] = ntFlags; // NT lowercase bits
    put16(img, off + 22, wtime);
    put16(img, off + 24, wdate);
    put16(img, off + 26, uint16_t(cluster & 0xFFFF));       // first cluster low
    put16(img, off + 20, uint16_t((cluster >> 16) & 0xFFFF)); // first cluster high
    put32(img, off + 28, size);
}

// One LFN slot (13 UTF-16 units) for names that fit a single slot.
void lfnEntry(std::vector<uint8_t>& img, size_t off, const char* utf8Name,
              const char name11[11])
{
    std::memset(img.data() + off, 0, 32);
    img[off] = 0x41; // seq 1 | last-part flag
    img[off + 11] = 0x0F;
    img[off + 13] = lfnChecksumOf(name11);
    // UTF-8 == ASCII for the test names used here.
    auto putUnit = [&](size_t unitIdx, uint16_t u) {
        size_t p;
        if (unitIdx < 5) {
            p = off + 1 + unitIdx * 2;
        } else if (unitIdx < 11) {
            p = off + 14 + (unitIdx - 5) * 2;
        } else {
            p = off + 28 + (unitIdx - 11) * 2;
        }
        img[p] = uint8_t(u & 0xFF);
        img[p + 1] = uint8_t(u >> 8);
    };
    int n = 0;
    for (; utf8Name[n] != '\0'; ++n) {
        putUnit(size_t(n), uint16_t(utf8Name[n]));
    }
    putUnit(size_t(n), 0x0000);
    for (int i = n + 1; i < 13; ++i) {
        putUnit(size_t(i), 0xFFFF);
    }
}

// A minimal FAT32 image: label + \EFI\Microsoft\Boot\bootmgfw.efi + 8.3 file.
std::vector<uint8_t> makeFat32Image()
{
    constexpr uint32_t kReserved = 4, kFatCount = 2, kFatSize = 8, kTotal = 64;
    std::vector<uint8_t> img(size_t(kTotal) * kSec, 0);
    // BPB.
    put16(img, 11, kSec);
    img[13] = 1;                  // sectors per cluster
    put16(img, 14, kReserved);
    img[16] = uint8_t(kFatCount);
    put16(img, 22, 0);            // fatSize16
    put32(img, 32, kTotal);
    put32(img, 36, kFatSize);     // fatSize32
    put32(img, 44, 2);            // root cluster
    img[510] = 0x55;
    img[511] = 0xAA;
    const uint32_t firstData = kReserved + kFatCount * kFatSize; // = 20
    for (uint32_t f = 0; f < kFatCount; ++f) {
        for (uint32_t c = 0; c < 8; ++c) {
            const uint32_t v = c == 0 ? 0x0FFFFFF8u : (c == 1 ? 0x0FFFFFFFu : 0x0FFFFFF8u);
            put32(img, size_t(kReserved + f * kFatSize) * kSec + c * 4, v);
        }
    }
    auto clusterSector = [&](uint32_t c) { return firstData + (c - 2); };

    // Root (cluster 2): volume label + EFI dir.
    shortEntry(img, size_t(clusterSector(2)) * kSec + 0, "MYESP      ", 0x08, 0, 0);
    shortEntry(img, size_t(clusterSector(2)) * kSec + 32, "EFI        ", 0x10, 3, 0);
    // EFI (cluster 3): LFN dir Microsoft + 8.3 file BOOTX64.EFI.
    lfnEntry(img, size_t(clusterSector(3)) * kSec + 0, "Microsoft", "MICROS~1   ");
    shortEntry(img, size_t(clusterSector(3)) * kSec + 32, "MICROS~1   ", 0x10, 4, 0);
    shortEntry(img, size_t(clusterSector(3)) * kSec + 64, "BOOTX64 EFI", 0x20, 6, 16);
    // Microsoft (cluster 4): Boot dir.
    shortEntry(img, size_t(clusterSector(4)) * kSec + 0, "Boot       ", 0x10, 5, 0);
    // Boot (cluster 5): bootmgfw.efi via NT lowercase flags (8 chars base).
    shortEntry(img, size_t(clusterSector(5)) * kSec + 0, "BOOTMGFWEFI", 0x20, 7, 128, 0x18);
    // File data.
    std::memcpy(img.data() + size_t(clusterSector(6)) * kSec, "BOOTX64-EFI-FILE", 16);
    for (int i = 0; i < 128; ++i) {
        img[size_t(clusterSector(7)) * kSec + size_t(i)] = uint8_t(i);
    }
    return img;
}

// A minimal FAT16 image: >= 4086 clusters (the FAT12/16 boundary), a fixed
// root directory and one 8.3 file.
std::vector<uint8_t> makeFat16Image()
{
    constexpr uint32_t kReserved = 1, kFatCount = 2, kFatSize = 17,
                       kRootEntries = 32, kData = 4100;
    const uint32_t kTotal = kReserved + kFatCount * kFatSize +
                            kRootEntries * 32 / kSec + kData; // = 4137
    std::vector<uint8_t> img(size_t(kTotal) * kSec, 0);
    put16(img, 11, kSec);
    img[13] = 1;
    put16(img, 14, kReserved);
    img[16] = uint8_t(kFatCount);
    put16(img, 17, kRootEntries);
    put16(img, 19, uint16_t(kTotal));
    put16(img, 22, kFatSize);
    img[510] = 0x55;
    img[511] = 0xAA;
    for (uint32_t f = 0; f < kFatCount; ++f) {
        put16(img, size_t(kReserved + f * kFatSize) * kSec + 0 * 2, 0xFFF8);
        put16(img, size_t(kReserved + f * kFatSize) * kSec + 1 * 2, 0xFFFF);
        put16(img, size_t(kReserved + f * kFatSize) * kSec + 2 * 2, 0xFFFF);
    }
    const uint32_t rootSector = kReserved + kFatCount * kFatSize; // = 35
    shortEntry(img, size_t(rootSector) * kSec + 0, "FAT16VOL   ", 0x08, 0, 0);
    shortEntry(img, size_t(rootSector) * kSec + 32, "KERNEL  SYS", 0x20, 2, 64);
    std::memcpy(img.data() + size_t(rootSector + 2) * kSec, "KERNEL.SYS-IMAGE", 16);
    return img;
}

FatVolume openVolume(const std::vector<uint8_t>& img, std::string* err = nullptr)
{
    FatVolume vol;
    std::string localErr;
    const bool ok = vol.open(
        [&img](uint64_t off, uint32_t bytes, uint8_t* out) {
            if (off + bytes > img.size()) {
                return false;
            }
            std::memcpy(out, img.data() + off, bytes);
            return true;
        },
        &localErr);
    if (err != nullptr) {
        *err = localErr;
    }
    REQUIRE(ok);
    return vol;
}

bool hasEntry(const std::vector<FatDirEntry>& v, const std::string& name)
{
    for (const FatDirEntry& e : v) {
        if (e.name == name) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("FAT32 volume opens and reports geometry")
{
    const std::vector<uint8_t> img = makeFat32Image();
    FatVolume vol = openVolume(img);
    CHECK(vol.isOpen());
    CHECK(vol.fatType() == "FAT32");
    CHECK(vol.bytesPerSector() == 512);
    CHECK(vol.totalBytes() == 64 * 512);
}

TEST_CASE("FAT32 root lists the volume label and directories")
{
    const std::vector<uint8_t> img = makeFat32Image();
    FatVolume vol = openVolume(img);
    std::string err;
    std::vector<FatDirEntry> entries;
    REQUIRE(vol.listDir("", &entries, &err));
    CHECK(err.empty());
    CHECK(hasEntry(entries, "MYESP"));
    CHECK(hasEntry(entries, "EFI"));
    const FatDirEntry* efi = nullptr;
    for (const FatDirEntry& e : entries) {
        if (e.name == "EFI") {
            efi = &e;
        }
        if (e.name == "MYESP") {
            CHECK(e.isVolumeLabel);
        }
    }
    REQUIRE(efi != nullptr);
    CHECK(efi->isDir);
}

TEST_CASE("FAT32 subdirectory listing resolves long file names")
{
    const std::vector<uint8_t> img = makeFat32Image();
    FatVolume vol = openVolume(img);
    std::string err;
    std::vector<FatDirEntry> entries;
    REQUIRE(vol.listDir("\\EFI", &entries, &err));
    CHECK(hasEntry(entries, "Microsoft")); // LFN
    CHECK(hasEntry(entries, "BOOTX64.EFI"));
    REQUIRE(vol.listDir("\\EFI\\Microsoft\\Boot", &entries, &err));
    CHECK(hasEntry(entries, "bootmgfw.efi")); // NT lowercase flags
}

TEST_CASE("FAT32 findEntry handles case, aliases and metadata")
{
    const std::vector<uint8_t> img = makeFat32Image();
    FatVolume vol = openVolume(img);
    FatDirEntry e;
    std::string err;
    REQUIRE(vol.findEntry("\\EFI\\Microsoft\\Boot\\bootmgfw.efi", &e, &err));
    CHECK(e.sizeBytes == 128);
    CHECK_FALSE(e.isDir);
    CHECK(e.mtime > 0); // valid FAT date 2025-09-20 12:34:56
    // Forward slashes and case-insensitive segments.
    REQUIRE(vol.findEntry("efi/microsoft/boot/BOOTMGFW.EFI", &e, &err));
    CHECK(e.sizeBytes == 128);
    // LFN lookup.
    REQUIRE(vol.findEntry("\\EFI\\Microsoft", &e, &err));
    CHECK(e.isDir);
    CHECK_FALSE(vol.findEntry("\\EFI\\Missing.efi", &e, &err));
}

TEST_CASE("FAT16 volume uses the fixed root directory")
{
    const std::vector<uint8_t> img = makeFat16Image();
    FatVolume vol = openVolume(img);
    CHECK(vol.isOpen());
    CHECK(vol.fatType() == "FAT16");
    std::string err;
    std::vector<FatDirEntry> entries;
    REQUIRE(vol.listDir("/", &entries, &err));
    CHECK(hasEntry(entries, "FAT16VOL"));
    CHECK(hasEntry(entries, "KERNEL.SYS"));
    FatDirEntry e;
    REQUIRE(vol.findEntry("\\kernel.sys", &e, &err));
    CHECK(e.sizeBytes == 64);
}

TEST_CASE("Non-FAT data is rejected with an error")
{
    std::vector<uint8_t> img(64 * 512, 0); // all zero: invalid BPB
    FatVolume vol;
    std::string err;
    CHECK_FALSE(vol.open(
        [&img](uint64_t off, uint32_t bytes, uint8_t* out) {
            std::memcpy(out, img.data() + off, bytes);
            return true;
        },
        &err));
    CHECK_FALSE(err.empty());
}
