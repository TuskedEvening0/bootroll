#include "core/disk/PartitionTable.h"

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

} // namespace bootroll
