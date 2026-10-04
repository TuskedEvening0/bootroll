// Unit tests for MBR detection/install and PBR detection/BPB patching (M3/M4).
#include <doctest.h>

#include "core/bootcode/BootCode.h"
#include "core/bootcode/PbrCode.h"

#include <cstring>
#include <vector>

using namespace bootroll;

namespace {

std::vector<uint8_t> sectorWithSignature()
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

// Target sector carrying a distinct disk signature + partition entry, so the
// preserve-tail assertions can tell staged bytes from target bytes.
std::vector<uint8_t> targetSector()
{
    std::vector<uint8_t> b = sectorWithSignature();
    for (int i = 0; i < 4; ++i) {
        b[0x1B8 + i] = uint8_t(0x10 + i);
    }
    b[0x1BE] = 0x80;
    b[0x1BE + 4] = 0x07;
    return b;
}

} // namespace

TEST_CASE("detectMbrKind: NT families split by INT13h extension opcodes")
{
    std::vector<uint8_t> b = sectorWithSignature();
    putAscii(b, 0x100, "Invalid partition table");

    SUBCASE("extended-read opcodes B4 41 -> NT6") {
        b[0x80] = 0xB4;
        b[0x81] = 0x41;
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Nt6);
    }
    SUBCASE("B4 42 -> NT6") {
        b[0x80] = 0xB4;
        b[0x81] = 0x42;
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Nt6);
    }
    SUBCASE("no extended reads -> NT5") {
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Nt5);
    }
}

TEST_CASE("detectMbrKind: string heuristics and signature gate")
{
    SUBCASE("GRUB string") {
        std::vector<uint8_t> b = sectorWithSignature();
        putAscii(b, 0x120, "GRUB");
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Grub4dos);
    }
    SUBCASE("Plop string") {
        std::vector<uint8_t> b = sectorWithSignature();
        putAscii(b, 0x0A0, "Plop");
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Plop);
    }
    SUBCASE("missing 55AA -> NotMbr") {
        std::vector<uint8_t> b(512, 0);
        putAscii(b, 0x120, "GRUB");
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::NotMbr);
    }
    SUBCASE("signature + unknown code -> Reactive") {
        std::vector<uint8_t> b = sectorWithSignature();
        b[0] = 0xEB;
        b[1] = 0x3C;
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Reactive);
    }
    SUBCASE("short buffer -> NotMbr") {
        uint8_t b[100] = {};
        CHECK(detectMbrKind(b, sizeof(b)) == MbrKind::NotMbr);
    }
}

TEST_CASE("mbrInstallInfo: bundled payload descriptors")
{
    SUBCASE("Nt6: single-sector, >= 440 code bytes") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Nt6, &info));
        CHECK_FALSE(info.multiSector);
        CHECK(info.blobSize >= 440);
        CHECK(info.blob[0x1FE] == 0x55); // blob carries its own signature
        CHECK(info.blob[0x1FF] == 0xAA);
    }
    SUBCASE("Grub4dos: 16-sector grldr.mbr") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Grub4dos, &info));
        CHECK(info.multiSector);
        CHECK(info.blobSize == 16 * 512);
        CHECK(info.blob[0x1FE] == 0x55);
        CHECK(info.blob[0x1FF] == 0xAA);
    }
    SUBCASE("Wee: just under a 63-sector track") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Wee, &info));
        CHECK(info.multiSector);
        CHECK(info.blobSize >= 512);
        CHECK(info.blobSize < 63 * 512); // staging zero-pads to a full track
    }
    SUBCASE("Syslinux: 440-byte single-sector code area") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Syslinux, &info));
        CHECK_FALSE(info.multiSector);
        CHECK(info.blobSize == 440);
    }
    SUBCASE("null out pointer is rejected") {
        CHECK_FALSE(mbrInstallInfo(MbrInstallType::Nt6, nullptr));
    }
}

TEST_CASE("applyMbrCode: replaces [0,440) and preserves [440,512)")
{
    std::vector<uint8_t> target = targetSector();

    MbrInstallInfo info;
    REQUIRE(mbrInstallInfo(MbrInstallType::Nt6, &info));
    REQUIRE(info.blobSize >= 440);

    REQUIRE(applyMbrCode(target.data(), target.size(), MbrInstallType::Nt6));

    // Code area now matches the blob; the tail is untouched.
    CHECK(std::memcmp(target.data(), info.blob, 440) == 0);
    CHECK(target[0x1B8] == 0x10); // disk signature preserved
    CHECK(target[0x1BB] == 0x13);
    CHECK(target[0x1BE] == 0x80); // active flag preserved
    CHECK(target[0x1BE + 4] == 0x07);
    CHECK(target[0x1FE] == 0x55);
    CHECK(target[0x1FF] == 0xAA);
    CHECK(detectMbrKind(target.data(), target.size()) == MbrKind::Nt6);

    SUBCASE("multi-sector types are rejected, sector untouched") {
        std::vector<uint8_t> b = sectorWithSignature();
        CHECK_FALSE(applyMbrCode(b.data(), b.size(), MbrInstallType::Grub4dos));
        CHECK_FALSE(applyMbrCode(b.data(), b.size(), MbrInstallType::Wee));
        CHECK(b[0] == 0); // untouched
    }
    SUBCASE("syslinux mbr.bin applies as a single-sector patch") {
        MbrInstallInfo sys;
        REQUIRE(mbrInstallInfo(MbrInstallType::Syslinux, &sys));
        std::vector<uint8_t> b = targetSector();
        REQUIRE(applyMbrCode(b.data(), b.size(), MbrInstallType::Syslinux));
        CHECK(std::memcmp(b.data(), sys.blob, 440) == 0);
        CHECK(b[0x1BE] == 0x80); // partition table preserved
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Syslinux);
    }
}

TEST_CASE("stageMbrInstall: single-sector types keep the target tail")
{
    std::vector<uint8_t> target = targetSector();

    SUBCASE("Nt6") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Nt6, &info));
        std::vector<uint8_t> staged;
        REQUIRE(stageMbrInstall(MbrInstallType::Nt6, target.data(),
                                target.size(), &staged));
        REQUIRE(staged.size() == 512);
        CHECK(std::memcmp(staged.data(), info.blob, 440) == 0);
        CHECK(std::memcmp(staged.data() + 440, target.data() + 440, 512 - 440) == 0);
    }
    SUBCASE("Syslinux (blob is exactly 440 bytes)") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Syslinux, &info));
        std::vector<uint8_t> staged;
        REQUIRE(stageMbrInstall(MbrInstallType::Syslinux, target.data(),
                                target.size(), &staged));
        REQUIRE(staged.size() == 512);
        CHECK(std::memcmp(staged.data(), info.blob, 440) == 0);
        CHECK(std::memcmp(staged.data() + 440, target.data() + 440, 512 - 440) == 0);
    }
    SUBCASE("rejects invalid input") {
        std::vector<uint8_t> staged;
        CHECK_FALSE(stageMbrInstall(MbrInstallType::Nt6, target.data(), 100, &staged));
        CHECK_FALSE(stageMbrInstall(MbrInstallType::Nt6, nullptr, 512, &staged));
        CHECK(staged.empty());
    }
}

TEST_CASE("stageMbrInstall: multi-sector types stage full payloads")
{
    std::vector<uint8_t> target = targetSector();
    std::vector<uint8_t> staged;

    SUBCASE("Grub4dos: 16 sectors, later sectors verbatim from the blob") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Grub4dos, &info));
        REQUIRE(stageMbrInstall(MbrInstallType::Grub4dos, target.data(),
                                target.size(), &staged));
        REQUIRE(staged.size() == info.blobSize);
        // Sector 0: code area from the blob, disk signature + DPT preserved.
        CHECK(std::memcmp(staged.data(), info.blob, 0x1B8) == 0);
        CHECK(std::memcmp(staged.data() + 0x1B8, target.data() + 0x1B8,
                          0x1FE - 0x1B8) == 0);
        // Sectors 1..15 come from the blob verbatim.
        CHECK(std::memcmp(staged.data() + 512, info.blob + 512,
                          info.blobSize - 512) == 0);
    }
    SUBCASE("Wee: zero-padded to a full 63-sector track") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Wee, &info));
        REQUIRE(stageMbrInstall(MbrInstallType::Wee, target.data(),
                                target.size(), &staged));
        REQUIRE(staged.size() == 63 * 512);
        CHECK(std::memcmp(staged.data(), info.blob, 0x1B8) == 0);
        CHECK(std::memcmp(staged.data() + 0x1B8, target.data() + 0x1B8,
                          0x1FE - 0x1B8) == 0);
        // Padding between the blob end and the track end is zeroed.
        CHECK(staged[info.blobSize] == 0);
        CHECK(staged[63 * 512 - 1] == 0);
    }
}

TEST_CASE("bundled blobs detect as their own kind")
{
    SUBCASE("syslinux mbr.bin + 55AA -> Syslinux (not NT, despite its NT strings)") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Syslinux, &info));
        std::vector<uint8_t> b(512, 0);
        std::memcpy(b.data(), info.blob, info.blobSize);
        b[0x1FE] = 0x55;
        b[0x1FF] = 0xAA;
        CHECK(detectMbrKind(b.data(), b.size()) == MbrKind::Syslinux);
    }
    SUBCASE("grldr.mbr sector 0 -> Grub4dos") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Grub4dos, &info));
        CHECK(detectMbrKind(info.blob, 512) == MbrKind::Grub4dos);
    }
    SUBCASE("wee63.mbr sector 0 -> Wee") {
        MbrInstallInfo info;
        REQUIRE(mbrInstallInfo(MbrInstallType::Wee, &info));
        CHECK(detectMbrKind(info.blob, 512) == MbrKind::Wee);
    }
    SUBCASE("grldr.pbr sector 0 -> Grub4dos PBR") {
        size_t n = 0;
        const uint8_t* blob = grub4dosPbrBlob(&n);
        REQUIRE(blob != nullptr);
        CHECK(n == 11 * 512);
        CHECK(detectPbrKind(blob, 512) == PbrKind::Grub4dos);
    }
    SUBCASE("GRLDR boot file is embedded for the Grub4DOS screen") {
        size_t n = 0;
        const uint8_t* blob = grub4dosGrldrBlob(&n);
        REQUIRE(blob != nullptr);
        CHECK(n > 100 * 1024); // real grldr image, not a stub
    }
}

TEST_CASE("detectPbrKind: filesystem families via OEM name and BPB")
{
    SUBCASE("NTFS") {
        std::vector<uint8_t> b = sectorWithSignature();
        putAscii(b, 0x03, "NTFS    ");
        CHECK(detectPbrKind(b.data(), b.size()) == PbrKind::Ntfs);
    }
    SUBCASE("exFAT") {
        std::vector<uint8_t> b = sectorWithSignature();
        putAscii(b, 0x03, "EXFAT   ");
        CHECK(detectPbrKind(b.data(), b.size()) == PbrKind::ExFat);
    }
    SUBCASE("FAT32 (fatSz32 set, fatSz16 zero)") {
        std::vector<uint8_t> b = sectorWithSignature();
        putAscii(b, 0x03, "MSDOS5.0");
        b[0x24] = 0x10; // fatSz32 = 0x10
        CHECK(detectPbrKind(b.data(), b.size()) == PbrKind::Fat32);
    }
    SUBCASE("FAT16 (fatSz16 set)") {
        std::vector<uint8_t> b = sectorWithSignature();
        putAscii(b, 0x03, "MSDOS5.0");
        b[0x16] = 0x20; // fatSz16
        CHECK(detectPbrKind(b.data(), b.size()) == PbrKind::Fat16);
    }
    SUBCASE("missing signature -> NotPbr") {
        std::vector<uint8_t> b(512, 0);
        putAscii(b, 0x03, "NTFS    ");
        CHECK(detectPbrKind(b.data(), b.size()) == PbrKind::NotPbr);
    }
}

TEST_CASE("BPB geometry patches hit the documented offsets only")
{
    SUBCASE("NTFS: QWORD hidden @0x1C, QWORD total @0x28") {
        std::vector<uint8_t> b(512, 0);
        b[0x1FE] = 0x55;
        b[0x1FF] = 0xAA;
        putAscii(b, 0x03, "NTFS    ");
        REQUIRE(patchNtfsBpb(b.data(), b.size(), 2048, 204800));

        uint64_t hidden = 0, total = 0;
        std::memcpy(&hidden, b.data() + 0x1C, 8);
        std::memcpy(&total, b.data() + 0x28, 8);
        CHECK(hidden == 2048);
        CHECK(total == 204800);
        for (int i = 0; i < 8; ++i) {
            if (i == 0x1C || i == 0x28) {
                continue; // patched geometry fields
            }
            if (i >= 0x03 && i < 0x0B) {
                continue; // OEM name "NTFS    " written by this test itself
            }
            CHECK(b[i] == 0); // nothing else touched
        }

        SUBCASE("rejects non-NTFS") {
            putAscii(b, 0x03, "FAT32   ");
            CHECK_FALSE(patchNtfsBpb(b.data(), b.size(), 1, 2));
        }
    }

    SUBCASE("FAT: DWORD hidden @0x1C, DWORD total32 @0x20, WORD total16 @0x13") {
        std::vector<uint8_t> b = sectorWithSignature();
        putAscii(b, 0x03, "MSDOS5.0");
        b[0x13] = 0xAB; // stale total16 gets recomputed
        REQUIRE(patchFatBpb(b.data(), b.size(), 128, 300000));

        uint32_t hidden = 0, total32 = 0;
        uint16_t total16 = 0;
        std::memcpy(&hidden, b.data() + 0x1C, 4);
        std::memcpy(&total32, b.data() + 0x20, 4);
        std::memcpy(&total16, b.data() + 0x13, 2);
        CHECK(hidden == 128);
        CHECK(total32 == 300000);
        CHECK(total16 == 0); // > 0xFFFE -> zeroed per FAT convention

        REQUIRE(patchFatBpb(b.data(), b.size(), 0, 10000));
        std::memcpy(&total16, b.data() + 0x13, 2);
        CHECK(total16 == 10000); // fits -> also mirrored into total16
    }

    SUBCASE("exFAT: QWORD offset @0x40, QWORD length @0x48") {
        std::vector<uint8_t> b(512, 0);
        b[0x1FE] = 0x55;
        b[0x1FF] = 0xAA;
        putAscii(b, 0x03, "EXFAT   ");
        REQUIRE(patchExFatBpb(b.data(), b.size(), 1024, 4096000));

        uint64_t v = 0;
        std::memcpy(&v, b.data() + 0x40, 8);
        CHECK(v == 1024);
        std::memcpy(&v, b.data() + 0x48, 8);
        CHECK(v == 4096000);

        SUBCASE("rejects non-exFAT") {
            putAscii(b, 0x03, "NTFS    ");
            CHECK_FALSE(patchExFatBpb(b.data(), b.size(), 1, 2));
        }
    }
}
