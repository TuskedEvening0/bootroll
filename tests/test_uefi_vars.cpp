// Unit tests for UEFI boot entry pack/parse, BootOrder codec and the backup
// text format (M7). Pure core logic - no OS access.
#include "core/uefi/UefiVars.h"

#include "doctest.h"

#include <cstring>

using namespace bootroll;

TEST_CASE("packLoadOption / parseLoadOption round-trip")
{
    const std::vector<uint8_t> raw = packLoadOption(
        1, "Windows Boot Manager", "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
    BootEntry e;
    REQUIRE(parseLoadOption(raw, &e));
    CHECK(e.attrs == 1);
    CHECK(e.desc == "Windows Boot Manager");
    CHECK(e.path == "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
    CHECK(e.raw == raw);
}

TEST_CASE("packLoadOption normalizes the file path")
{
    BootEntry e;
    REQUIRE(parseLoadOption(packLoadOption(0, "X", "EFI/Boot/bootx64.efi"), &e));
    CHECK(e.path == "\\EFI\\Boot\\bootx64.efi");
}

TEST_CASE("parseLoadOption keeps trailing optional data in raw but not in path")
{
    std::vector<uint8_t> raw = packLoadOption(1, "D", "\\a.efi");
    raw.push_back(0xDE);
    raw.push_back(0xAD); // OptionalData after the end-of-device-path node
    BootEntry e;
    REQUIRE(parseLoadOption(raw, &e));
    CHECK(e.desc == "D");
    CHECK(e.path == "\\a.efi");
    CHECK(e.raw == raw);
}

TEST_CASE("parseLoadOption rejects truncated headers")
{
    BootEntry e;
    CHECK_FALSE(parseLoadOption({}, &e));
    CHECK_FALSE(parseLoadOption({1, 2, 3, 4, 5}, &e));
}

TEST_CASE("parseLoadOption survives a missing description terminator")
{
    // attrs(4) + fpLen(2) + UTF-16 "AB" with no NUL: the description must be
    // truncated at the declared FilePathList region instead of running wild.
    const std::vector<uint8_t> raw = { 0x01, 0x00, 0x00, 0x00, 0x08, 0x00,
                                       0x41, 0x00, 0x42, 0x00 };
    BootEntry e;
    REQUIRE(parseLoadOption(raw, &e));
    CHECK(e.desc == "AB");
    CHECK(e.path.empty());
}

TEST_CASE("parseLoadOption stops at a bogus device path node length")
{
    std::vector<uint8_t> raw = packLoadOption(1, "D", "\\a.efi");
    REQUIRE(raw.size() >= 14);
    // Corrupt the file-path node length (bytes right after the description).
    raw[10] = 0x04;
    raw[11] = 0x04;
    raw[12] = 0xFF;
    raw[13] = 0xFF;
    BootEntry e;
    REQUIRE(parseLoadOption(raw, &e)); // must not loop or crash
    CHECK(e.desc == "D");
    CHECK(e.path.empty());
}

TEST_CASE("BootOrder codec round-trips and drops odd trailing bytes")
{
    const std::vector<uint8_t> bytes = encodeBootOrder({0x0000, 0x0001, 0x03FF});
    REQUIRE(bytes.size() == 6);
    const std::vector<uint16_t> order = decodeBootOrder(bytes);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 0x0000);
    CHECK(order[1] == 0x0001);
    CHECK(order[2] == 0x03FF);

    std::vector<uint8_t> odd = bytes;
    odd.push_back(0x42); // truncated tail unit must be ignored
    CHECK(decodeBootOrder(odd).size() == 3);
}

TEST_CASE("nextFreeBootNumber picks the smallest unused slot")
{
    CHECK(nextFreeBootNumber({}) == 0);
    CHECK(nextFreeBootNumber({0, 1, 3}) == 2);
    std::set<uint16_t> dense;
    for (int i = 0; i < 0xFFFF; ++i) {
        dense.insert(uint16_t(i));
    }
    CHECK(nextFreeBootNumber(dense) == 0xFFFF);
}

TEST_CASE("Boot variable names round-trip")
{
    CHECK(bootVarName(0) == "Boot0000");
    CHECK(bootVarName(0x3FF) == "Boot03FF");
    uint16_t n = 0xFFFF;
    REQUIRE(parseBootVarName(bootVarName(0x1234), &n));
    CHECK(n == 0x1234);
    REQUIRE(parseBootVarName("boot000a", &n)); // case-insensitive prefix and hex
    CHECK(n == 0x000A);
    CHECK_FALSE(parseBootVarName("", &n));
    CHECK_FALSE(parseBootVarName("Boot12", &n));
    CHECK_FALSE(parseBootVarName("Boot12345", &n));
    CHECK_FALSE(parseBootVarName("Boot00XY", &n));
    CHECK_FALSE(parseBootVarName("Fast0001", &n));
}

TEST_CASE("UEFI backup text round-trip")
{
    UefiBackup b;
    b.hasTimeout = true;
    b.timeout = 3;
    b.hasBootNext = true;
    b.bootNext = 0x0002;
    b.bootOrder = {0x0001, 0x0002, 0x0000};
    b.entries[0x0001] = packLoadOption(1, "First", "\\first.efi");
    b.entries[0x0002] = packLoadOption(0, "Second", "\\second.efi");

    const std::string text = writeUefiBackupText(b);
    UefiBackup r;
    REQUIRE(parseUefiBackupText(text, &r));
    CHECK(r.hasTimeout);
    CHECK(r.timeout == 3);
    CHECK(r.hasBootNext);
    CHECK(r.bootNext == 0x0002);
    CHECK(r.bootOrder == b.bootOrder);
    REQUIRE(r.entries.size() == 2);
    CHECK(r.entries.at(0x0001) == b.entries.at(0x0001));
    CHECK(r.entries.at(0x0002) == b.entries.at(0x0002));
}

TEST_CASE("UEFI backup text tolerates comments, rejects malformed files")
{
    UefiBackup r;
    CHECK(parseUefiBackupText("# comment\r\n\r\nbootorder=0001\r\n", &r));
    CHECK(r.bootOrder.size() == 1);
    CHECK(r.bootOrder[0] == 0x0001);
    CHECK_FALSE(r.hasTimeout);
    CHECK_FALSE(r.hasBootNext);

    UefiBackup bad;
    CHECK_FALSE(parseUefiBackupText("raw=XYZ\n", &bad)); // raw= outside an entry
    CHECK_FALSE(parseUefiBackupText("[Boot0001]\nraw=0G\n", &bad)); // bad hex digit
    CHECK_FALSE(parseUefiBackupText("[Boot0001]\nraw=0\n", &bad)); // odd hex length
    CHECK_FALSE(parseUefiBackupText("wat=1\n", &bad)); // unknown line
    CHECK_FALSE(parseUefiBackupText("bootorder=01\n", &bad)); // token != 4 hex
    CHECK_FALSE(parseUefiBackupText("[BootX]\n", &bad)); // malformed header
    CHECK_FALSE(parseUefiBackupText("timeout=70000\n", &bad)); // out of range
}

TEST_CASE("parseLoadOption accepts spec MEDIA_HARDDRIVE_DP subType 1 and legacy 3")
{
    HdPathSpec hd;
    hd.partition = 1;
    auto raw = packLoadOptionFull(0x1, "X", &hd, "\\boot.efi");
    REQUIRE(raw.size() > 12);
    BootEntry e;
    REQUIRE(parseLoadOption(raw, &e));
    CHECK(e.hasHdNode);
    CHECK(e.firstDpType == 4);
    CHECK(e.firstDpSubType == 1);
    CHECK(devicePathTypeLabel(4, 1) == "Media Device Path (0x04) - Hard Drive");
    CHECK(devicePathTypeLabel(3, 0) == "Messaging Device Path (0x03)");
    // Entries written by pre-2026-10-04 bootroll builds carry subType 3
    // (a bootroll bug); the parser still accepts those.
    raw[11] = 3;
    BootEntry legacy;
    REQUIRE(parseLoadOption(raw, &legacy));
    CHECK(legacy.hasHdNode);
    CHECK(legacy.hdPartition == 1);
}

TEST_CASE("packLoadOptionFull embeds a MEDIA_HARDDRIVE_DP node")
{
    HdPathSpec hd;
    hd.partition = 2;
    hd.startLba = 2048;
    hd.sizeLba = 1024000;
    hd.signature = {0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
                    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    hd.signatureType = 2; // GPT partition GUID
    const std::vector<uint8_t> raw =
        packLoadOptionFull(0x5, "Ubuntu", &hd, "\\EFI\\ubuntu\\grubx64.efi");
    BootEntry e;
    REQUIRE(parseLoadOption(raw, &e));
    CHECK(e.attrs == 0x5);
    CHECK(e.desc == "Ubuntu");
    CHECK(e.path == "\\EFI\\ubuntu\\grubx64.efi");
    CHECK(e.hasHdNode);
    CHECK(e.hdPartition == 2);
    CHECK(e.hdPartStart == 2048);
    CHECK(e.hdPartSize == 1024000);
    REQUIRE(e.hdSignature.size() == 16);
    CHECK(std::memcmp(e.hdSignature.data(), hd.signature.data(), 16) == 0);
    CHECK(e.hdSignatureType == 2);
}

TEST_CASE("packLoadOptionFull MBR signature type keeps the signature")
{
    HdPathSpec hd;
    hd.partition = 1;
    hd.startLba = 63;
    hd.sizeLba = 204800;
    hd.signature = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
                    0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
    hd.signatureType = 1; // MBR disk signature
    BootEntry e;
    REQUIRE(parseLoadOption(packLoadOptionFull(1, "HDD", &hd, "\\boot.efi"), &e));
    CHECK(e.hasHdNode);
    CHECK(e.hdSignatureType == 1);
    CHECK(e.hdPartition == 1);
    CHECK(e.hdPartStart == 63);
    CHECK(e.hdSignature.size() == 16);
    CHECK(e.hdSignature[0] == 0xDE);
    CHECK(e.hdSignature[3] == 0xEF);
}

TEST_CASE("packLoadOption without an HD node parses without one")
{
    BootEntry e;
    REQUIRE(parseLoadOption(packLoadOption(1, "X", "\\a.efi"), &e));
    CHECK_FALSE(e.hasHdNode);
    CHECK(e.hdSignature.empty());
    CHECK(e.hdSignatureType == 0);
}
