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
