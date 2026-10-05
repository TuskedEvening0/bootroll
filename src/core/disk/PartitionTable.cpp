#include "core/disk/PartitionTable.h"

#include "core/bcd/Utf16.h"

#include <cstdio>
#include <cstring>

namespace bootroll {

namespace {
constexpr size_t kEntryBase = 0x1BE;
constexpr size_t kEntrySize = 16;
constexpr size_t kEntryCount = 4;
} // namespace

std::string_view mbrPartitionTypeLabel(uint8_t type)
{
    switch (type) {
    case 0x00: return "Empty";
    case 0x01: return "FAT12";
    case 0x11: return "FAT12 (hidden)";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x14: case 0x16: case 0x1E: return "FAT16 (hidden)";
    case 0x05: case 0x0F: return "Extended";
    case 0x07: return "NTFS/exFAT";
    case 0x17: return "NTFS/exFAT (hidden)";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x1B: case 0x1C: return "FAT32 (hidden)";
    case 0x12: return "EISA / Diagnostic";
    case 0x27: return "NT Hidden / WinRE";
    case 0x2A: return "AtheOS";
    case 0x42: return "LDM / Dynamic";
    case 0x83: return "Linux";
    case 0x82: return "Linux swap";
    case 0x8E: return "Linux LVM";
    case 0xA5: return "FreeBSD";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System";
    default: return {};
    }
}

MbrTable parseMbr(const uint8_t* sector, size_t size)
{
    MbrTable t;
    t.entries.resize(kEntryCount);
    if (size < 512)
        return t;
    t.bootSignature = sector[0x1FE] == 0x55 && sector[0x1FF] == 0xAA;
    t.diskSignature = uint32_t(sector[0x1B8]) | uint32_t(sector[0x1B9]) << 8 |
                      uint32_t(sector[0x1BA]) << 16 | uint32_t(sector[0x1BB]) << 24;
    for (size_t i = 0; i < kEntryCount; ++i) {
        const uint8_t* e = sector + kEntryBase + i * kEntrySize;
        MbrEntry& m = t.entries[i];
        m.active = e[0] == 0x80;
        m.type = e[4];
        m.beginLba = uint32_t(e[8]) | uint32_t(e[9]) << 8 | uint32_t(e[10]) << 16 |
                     uint32_t(e[11]) << 24;
        m.sectorCount = uint32_t(e[12]) | uint32_t(e[13]) << 8 | uint32_t(e[14]) << 16 |
                        uint32_t(e[15]) << 24;
        m.empty = m.type == 0 && m.sectorCount == 0;
    }
    return t;
}

bool setMbrActiveEntry(uint8_t* sector, size_t size, int index, bool active)
{
    if (size < 512 || index < 0 || index >= (int)kEntryCount)
        return false;
    uint8_t* e = sector + kEntryBase + size_t(index) * kEntrySize;
    e[0] = active ? 0x80 : 0x00;
    return true;
}

bool setMbrActiveExclusive(uint8_t* sector, size_t size, int index)
{
    if (size < 512 || index < 0 || index >= (int)kEntryCount)
        return false;
    for (int i = 0; i < (int)kEntryCount; ++i)
        setMbrActiveEntry(sector, size, i, i == index);
    return true;
}

bool setMbrEntryType(uint8_t* sector, size_t size, int index, uint8_t type)
{
    if (size < 512 || index < 0 || index >= (int)kEntryCount)
        return false;
    sector[kEntryBase + size_t(index) * kEntrySize + 4] = type;
    return true;
}

bool clearMbrEntry(uint8_t* sector, size_t size, int index)
{
    if (size < 512 || index < 0 || index >= (int)kEntryCount)
        return false;
    uint8_t* e = sector + kEntryBase + size_t(index) * kEntrySize;
    std::memset(e, 0, kEntrySize);
    return true;
}

uint8_t mbrHiddenTypeOf(uint8_t type)
{
    switch (type) {
    case 0x01: case 0x04: case 0x06: case 0x07:
    case 0x0B: case 0x0C: case 0x0E:
        return uint8_t(type | 0x10);
    default:
        return 0;
    }
}

uint8_t mbrVisibleTypeOf(uint8_t type)
{
    switch (type) {
    case 0x11: case 0x14: case 0x16: case 0x17:
    case 0x1B: case 0x1C: case 0x1E:
        return uint8_t(type & 0x0F);
    default:
        return 0;
    }
}

// --- GPT ----------------------------------------------------------------

namespace {

constexpr size_t kGptEntryMinSize = 128;
constexpr uint64_t kGptEntryNameBytes = 72; // 36 UTF-16LE code units

uint32_t readLe32(const uint8_t* p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}

uint64_t readLe64(const uint8_t* p)
{
    return uint64_t(readLe32(p)) | uint64_t(readLe32(p + 4)) << 32;
}

// Mixed-endian GUID bytes -> canonical lowercase "xxxxxxxx-xxxx-..." string.
std::string guidToString(const uint8_t* b)
{
    const uint32_t data1 = readLe32(b);
    const uint16_t data2 = uint16_t(b[4]) | uint16_t(b[5]) << 8;
    const uint16_t data3 = uint16_t(b[6]) | uint16_t(b[7]) << 8;
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  data1, data2, data3, b[8], b[9], b[10], b[11], b[12], b[13],
                  b[14], b[15]);
    return buf;
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t size)
{
    // IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320) - as used by GPT.
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = uint32_t(0u - (crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

GptHeader parseGptHeader(const uint8_t* sector, size_t size)
{
    GptHeader h;
    if (size < 92 || std::memcmp(sector, "EFI PART", 8) != 0) {
        return h;
    }
    const uint32_t storedCrc = readLe32(sector + 0x10);
    const uint32_t declaredSize = readLe32(sector + 0x0C);
    if (declaredSize < 92 || declaredSize > size) {
        return h;
    }
    // Header CRC32 covers HeaderSize bytes with the CRC field zeroed.
    std::vector<uint8_t> tmp(sector, sector + declaredSize);
    std::memset(tmp.data() + 0x10, 0, 4);
    if (crc32(tmp.data(), tmp.size()) != storedCrc) {
        return h;
    }

    h.valid = true;
    h.headerSize = declaredSize;
    h.currentLba = readLe64(sector + 0x18);
    h.backupLba = readLe64(sector + 0x20);
    h.firstUsableLba = readLe64(sector + 0x28);
    h.lastUsableLba = readLe64(sector + 0x30);
    h.entryArrayLba = readLe64(sector + 0x48);
    h.entryCount = readLe32(sector + 0x50);
    h.entrySize = readLe32(sector + 0x54);
    h.entryCrc = readLe32(sector + 0x58);
    return h;
}

std::vector<GptEntry> parseGptEntries(const GptHeader& header,
                                      const uint8_t* bytes, size_t size)
{
    std::vector<GptEntry> out;
    if (!header.valid || header.entrySize < kGptEntryMinSize) {
        return out;
    }
    const size_t needed = size_t(header.entryCount) * header.entrySize;
    if (header.entryCount == 0 || size < needed) {
        return out;
    }
    if (crc32(bytes, needed) != header.entryCrc) {
        return out; // entry array corrupt
    }
    out.reserve(header.entryCount);
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        const uint8_t* e = bytes + size_t(i) * header.entrySize;
        static constexpr uint8_t kZeroGuid[16] = {};
        if (std::memcmp(e, kZeroGuid, 16) == 0) {
            continue; // unused entry
        }
        GptEntry ge;
        ge.typeGuid = guidToString(e);
        ge.partGuid = guidToString(e + 16);
        ge.firstLba = readLe64(e + 32);
        ge.lastLba = readLe64(e + 40);
        ge.attributes = readLe64(e + 48);
        // Name is NUL-padded UTF-16LE: cut at the first zero code unit.
        size_t nameUnits = size_t(kGptEntryNameBytes) / 2;
        for (size_t u = 0; u < nameUnits; ++u) {
            if (e[56 + 2 * u] == 0 && e[56 + 2 * u + 1] == 0) {
                nameUnits = u;
                break;
            }
        }
        ge.name = utf16leToUtf8(e + 56, nameUnits * 2);
        out.push_back(std::move(ge));
    }
    return out;
}

} // namespace bootroll
