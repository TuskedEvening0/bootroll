// Unit tests for the MBR partition table parser/editor (M3/M4).
#include <doctest.h>

#include "core/disk/PartitionTable.h"

#include <cstring>
#include <vector>

using bootroll::clearMbrEntry;
using bootroll::mbrHiddenTypeOf;
using bootroll::mbrPartitionTypeLabel;
using bootroll::mbrVisibleTypeOf;
using bootroll::parseMbr;
using bootroll::setMbrActiveEntry;
using bootroll::setMbrActiveExclusive;
using bootroll::setMbrEntryType;

namespace {

// Builds a 512-byte sector with a boot signature, a disk signature and one
// active NTFS partition (LBA 2048, 100 MiB) in entry 0.
std::vector<uint8_t> sampleMbr()
{
    std::vector<uint8_t> b(512, 0);
    b[0x1FE] = 0x55;
    b[0x1FF] = 0xAA;
    b[0x1B8] = 0x44; // disk signature 0xAABBCC44 little endian
    b[0x1B9] = 0xCC;
    b[0x1BA] = 0xBB;
    b[0x1BB] = 0xAA;

    uint8_t* e = b.data() + 0x1BE;
    e[0] = 0x80;             // active
    e[4] = 0x07;             // NTFS/exFAT
    const uint32_t begin = 2048;
    const uint32_t count = 204800;
    for (int i = 0; i < 4; ++i) {
        e[8 + i] = uint8_t(begin >> (8 * i));
        e[12 + i] = uint8_t(count >> (8 * i));
    }
    return b;
}

} // namespace

TEST_CASE("parseMbr: signature, disk signature and entries")
{
    const std::vector<uint8_t> b = sampleMbr();
    const bootroll::MbrTable t = parseMbr(b.data(), b.size());

    CHECK(t.bootSignature);
    CHECK(t.diskSignature == 0xAABBCC44u);
    REQUIRE(t.entries.size() == 4);

    CHECK(t.entries[0].active);
    CHECK_FALSE(t.entries[0].empty);
    CHECK(t.entries[0].type == 0x07);
    CHECK(t.entries[0].beginLba == 2048u);
    CHECK(t.entries[0].sectorCount == 204800u);

    for (int i = 1; i < 4; ++i) {
        CHECK(t.entries[i].empty);
        CHECK(t.entries[i].type == 0);
    }
}

TEST_CASE("parseMbr: short and unsigned sectors are safe")
{
    const uint8_t junk[16] = {};
    const bootroll::MbrTable t = parseMbr(junk, sizeof(junk));
    CHECK_FALSE(t.bootSignature);
    CHECK(t.entries.size() == 4);
    for (const auto& e : t.entries) {
        CHECK(e.empty);
    }
}

TEST_CASE("setMbrActiveEntry toggles exactly one flag byte")
{
    std::vector<uint8_t> b = sampleMbr();

    CHECK(setMbrActiveEntry(b.data(), b.size(), 2, true));
    CHECK(b[0x1BE] == 0x80);           // untouched
    CHECK(b[0x1BE + 32] == 0x80);      // entry 2 now active
    CHECK(parseMbr(b.data(), b.size()).entries[2].active);

    CHECK(setMbrActiveEntry(b.data(), b.size(), 2, false));
    CHECK(b[0x1BE + 32] == 0x00);

    CHECK_FALSE(setMbrActiveEntry(b.data(), b.size(), 4, true));
    CHECK_FALSE(setMbrActiveEntry(b.data(), 100, 0, true));
}

TEST_CASE("partition type labels")
{
    CHECK(std::string(mbrPartitionTypeLabel(0x07)) == "NTFS/exFAT");
    CHECK(std::string(mbrPartitionTypeLabel(0x0C)) == "FAT32");
    CHECK(std::string(mbrPartitionTypeLabel(0x17)) == "NTFS/exFAT (hidden)");
    CHECK(std::string(mbrPartitionTypeLabel(0xEE)) == "GPT protective");
    CHECK(mbrPartitionTypeLabel(0x99).empty());
}

TEST_CASE("setMbrActiveExclusive marks one entry and clears the others")
{
    std::vector<uint8_t> b = sampleMbr();

    CHECK(setMbrActiveExclusive(b.data(), b.size(), 2));
    const bootroll::MbrTable t = parseMbr(b.data(), b.size());
    CHECK_FALSE(t.entries[0].active);   // entry 0 was active in the sample
    CHECK(t.entries[2].active);
    CHECK_FALSE(t.entries[1].active);
    CHECK_FALSE(t.entries[3].active);
    // Nothing else about the entries changed.
    CHECK(t.entries[0].type == 0x07);
    CHECK(t.entries[0].beginLba == 2048u);
    CHECK(t.bootSignature);

    CHECK_FALSE(setMbrActiveExclusive(b.data(), b.size(), 4));
    CHECK_FALSE(setMbrActiveExclusive(b.data(), 100, 0));
}

TEST_CASE("setMbrEntryType changes exactly the type byte")
{
    std::vector<uint8_t> b = sampleMbr();

    CHECK(setMbrEntryType(b.data(), b.size(), 0, 0x1B));
    CHECK(b[0x1BE + 4] == 0x1B);
    const bootroll::MbrTable t = parseMbr(b.data(), b.size());
    CHECK(t.entries[0].type == 0x1B);
    CHECK(t.entries[0].active);          // untouched
    CHECK(t.entries[0].beginLba == 2048u);

    CHECK_FALSE(setMbrEntryType(b.data(), b.size(), 4, 0x07));
    CHECK_FALSE(setMbrEntryType(b.data(), 100, 0, 0x07));
}

TEST_CASE("clearMbrEntry zeroes the whole slot and keeps others")
{
    std::vector<uint8_t> b = sampleMbr();
    REQUIRE(setMbrEntryType(b.data(), b.size(), 1, 0x0C));

    CHECK(clearMbrEntry(b.data(), b.size(), 0));
    const bootroll::MbrTable t = parseMbr(b.data(), b.size());
    CHECK(t.entries[0].empty);
    CHECK(t.entries[0].type == 0);
    CHECK_FALSE(t.entries[0].active);
    CHECK(t.entries[0].beginLba == 0u);
    CHECK(t.entries[0].sectorCount == 0u);
    // Slot 1 survived, disk/boot signatures untouched.
    CHECK(t.entries[1].type == 0x0C);
    CHECK(t.entries[1].beginLba == 0u); // sample leaves entry 1 LBA zeroed
    CHECK(t.diskSignature == 0xAABBCC44u);
    CHECK(t.bootSignature);
    // Raw bytes of slot 0 are all zero.
    const uint8_t* e0 = b.data() + 0x1BE;
    for (int i = 0; i < 16; ++i) {
        CHECK(e0[i] == 0);
    }

    CHECK_FALSE(clearMbrEntry(b.data(), b.size(), -1));
    CHECK_FALSE(clearMbrEntry(b.data(), 100, 0));
}

TEST_CASE("hidden <-> visible type mapping")
{
    // Visible types get the 0x10 bit; only the planned set maps.
    CHECK(mbrHiddenTypeOf(0x07) == 0x17);
    CHECK(mbrHiddenTypeOf(0x0B) == 0x1B);
    CHECK(mbrHiddenTypeOf(0x0C) == 0x1C);
    CHECK(mbrHiddenTypeOf(0x06) == 0x16);
    CHECK(mbrHiddenTypeOf(0x0E) == 0x1E);
    CHECK(mbrHiddenTypeOf(0x01) == 0x11);
    CHECK(mbrHiddenTypeOf(0x04) == 0x14);
    CHECK(mbrHiddenTypeOf(0x83) == 0);
    CHECK(mbrHiddenTypeOf(0x17) == 0); // already hidden
    CHECK(mbrHiddenTypeOf(0x05) == 0); // extended never hidden

    CHECK(mbrVisibleTypeOf(0x17) == 0x07);
    CHECK(mbrVisibleTypeOf(0x1B) == 0x0B);
    CHECK(mbrVisibleTypeOf(0x1C) == 0x0C);
    CHECK(mbrVisibleTypeOf(0x16) == 0x06);
    CHECK(mbrVisibleTypeOf(0x1E) == 0x0E);
    CHECK(mbrVisibleTypeOf(0x11) == 0x01);
    CHECK(mbrVisibleTypeOf(0x14) == 0x04);
    CHECK(mbrVisibleTypeOf(0x07) == 0); // already visible
    CHECK(mbrVisibleTypeOf(0x83) == 0);

    // Round trip through parse/edit/parse.
    std::vector<uint8_t> b = sampleMbr();
    REQUIRE(setMbrEntryType(b.data(), b.size(), 0, mbrHiddenTypeOf(0x07)));
    CHECK(parseMbr(b.data(), b.size()).entries[0].type == 0x17);
    REQUIRE(setMbrEntryType(b.data(), b.size(), 0, mbrVisibleTypeOf(0x17)));
    CHECK(parseMbr(b.data(), b.size()).entries[0].type == 0x07);
}

// --- GPT (M8) ------------------------------------------------------------

#include "core/disk/PartitionTable.h"

#include <cstdio>

using bootroll::crc32;
using bootroll::parseGptEntries;
using bootroll::parseGptHeader;

namespace {

void putLe32(std::vector<uint8_t>& b, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        b[off + size_t(i)] = uint8_t(v >> (8 * i));
    }
}

void putLe64(std::vector<uint8_t>& b, size_t off, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        b[off + size_t(i)] = uint8_t(v >> (8 * i));
    }
}

// GUID "c12a7328-f81f-11d2-ba4b-00a0c93ec93b" in mixed-endian byte layout.
std::vector<uint8_t> espTypeGuidBytes()
{
    return {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
            0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};
}

struct GptFixture {
    std::vector<uint8_t> header = std::vector<uint8_t>(512, 0);
    std::vector<uint8_t> entries = std::vector<uint8_t>(128 * 4, 0);

    void writeHeader(uint32_t entryCount, uint32_t entrySize)
    {
        std::memcpy(header.data(), "EFI PART", 8);
        putLe32(header, 0x0C, 92);          // header size
        putLe32(header, 0x10, 0);           // CRC (fixed up below)
        putLe64(header, 0x18, 1);           // current LBA
        putLe64(header, 0x20, 0x12345678);  // backup LBA
        putLe64(header, 0x28, 34);          // first usable
        putLe64(header, 0x30, 1000);        // last usable
        putLe64(header, 0x48, 2);           // entry array LBA
        putLe32(header, 0x50, entryCount);
        putLe32(header, 0x54, entrySize);
        putLe32(header, 0x58, crc32(entries.data(), entryCount * entrySize));
        putLe32(header, 0x10, crc32(header.data(), 92));
    }

    void writeEntry(size_t index, const std::vector<uint8_t>& typeGuid,
                    uint64_t firstLba, uint64_t lastLba, const char* name)
    {
        uint8_t* e = entries.data() + index * 128;
        std::memcpy(e, typeGuid.data(), 16);
        for (int i = 0; i < 16; ++i) {
            e[16 + i] = uint8_t(0x10 * (index + 1) + i); // unique partition guid
        }
        putLe64(entries, index * 128 + 32, firstLba);
        putLe64(entries, index * 128 + 40, lastLba);
        // UTF-16LE name; ASCII input only.
        for (int i = 0; name[i] != 0 && i < 36; ++i) {
            e[56 + 2 * i] = uint8_t(name[i]);
        }
    }
};

} // namespace

TEST_CASE("crc32 matches the IEEE 802.3 check value")
{
    CHECK(crc32(nullptr, 0) == 0x00000000u);
    const uint8_t data[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc32(data, sizeof(data)) == 0xCBF43926u); // canonical check value
}

TEST_CASE("parseGptHeader: rejects non-GPT sectors")
{
    bootroll::GptHeader h = parseGptHeader(sampleMbr().data(), 512);
    CHECK_FALSE(h.valid);
    std::vector<uint8_t> sig(512, 0);
    std::memcpy(sig.data(), "EFI PARTX", 9); // truncated signature
    h = parseGptHeader(sig.data(), 512);
    CHECK_FALSE(h.valid);
}

TEST_CASE("parseGptHeader + entries: full round trip")
{
    GptFixture fx;
    fx.writeEntry(0, espTypeGuidBytes(), 2048, 6758399, "EFI system partition");
    fx.writeEntry(1, espTypeGuidBytes(), 6758400, 100020000, "basic data");
    fx.writeHeader(4, 128); // 2 used + 2 zeroed (unused) entries

    const bootroll::GptHeader h = parseGptHeader(fx.header.data(), 512);
    REQUIRE(h.valid);
    CHECK(h.entryCount == 4);
    CHECK(h.entrySize == 128);
    CHECK(h.entryArrayLba == 2);
    CHECK(h.firstUsableLba == 34);
    CHECK(h.lastUsableLba == 1000);

    const std::vector<bootroll::GptEntry> entries =
        parseGptEntries(h, fx.entries.data(), fx.entries.size());
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].typeGuid == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b");
    CHECK(entries[0].firstLba == 2048);
    CHECK(entries[0].lastLba == 6758399);
    CHECK(entries[0].name == "EFI system partition");
    CHECK(entries[1].partGuid.size() == 36);
    CHECK(entries[1].name == "basic data");
}

TEST_CASE("parseGptEntries: corrupt array CRC yields no entries")
{
    GptFixture fx;
    fx.writeEntry(0, espTypeGuidBytes(), 2048, 6758399, "ESP");
    fx.writeHeader(4, 128);
    fx.entries[60] ^= 0xFF; // flip a byte inside the entry array

    const bootroll::GptHeader h = parseGptHeader(fx.header.data(), 512);
    REQUIRE(h.valid);
    CHECK(parseGptEntries(h, fx.entries.data(), fx.entries.size()).empty());
}

TEST_CASE("parseGptHeader: corrupt header CRC is rejected")
{
    GptFixture fx;
    fx.writeEntry(0, espTypeGuidBytes(), 2048, 6758399, "ESP");
    fx.writeHeader(4, 128);
    fx.header[70] ^= 0xFF; // flip a payload byte after the CRC was fixed up

    const bootroll::GptHeader h = parseGptHeader(fx.header.data(), 512);
    CHECK_FALSE(h.valid);
}
