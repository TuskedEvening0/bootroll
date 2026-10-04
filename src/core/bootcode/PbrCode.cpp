#include "core/bootcode/PbrCode.h"

#include <cstring>

namespace bootroll {

namespace {

bool hasSignature(const uint8_t* sector, size_t size)
{
    return size >= 512 && sector[0x1FE] == 0x55 && sector[0x1FF] == 0xAA;
}

bool oemIs(const uint8_t* sector, const char* name)
{
    return std::memcmp(sector + 0x03, name, std::strlen(name)) == 0;
}

bool containsAscii(const uint8_t* p, size_t size, const char* s)
{
    size_t n = std::strlen(s);
    if (size < n)
        return false;
    for (size_t i = 0; i + n <= size; ++i)
        if (std::memcmp(p + i, s, n) == 0)
            return true;
    return false;
}

void putLe16(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

void putLe32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = uint8_t(v >> (8 * i));
}

void putLe64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        p[i] = uint8_t(v >> (8 * i));
}

} // namespace

std::string_view pbrKindLabel(PbrKind k)
{
    switch (k) {
    case PbrKind::Ntfs: return "NTFS boot sector";
    case PbrKind::Fat12: return "FAT12 boot sector";
    case PbrKind::Fat16: return "FAT16 boot sector";
    case PbrKind::Fat32: return "FAT32 boot sector";
    case PbrKind::ExFat: return "exFAT boot sector";
    case PbrKind::Grub4dos: return "Grub4DOS PBR";
    case PbrKind::NotPbr: return "No boot signature";
    default: return "Unknown format";
    }
}

PbrKind detectPbrKind(const uint8_t* sector, size_t size)
{
    if (!hasSignature(sector, size))
        return PbrKind::NotPbr;
    if (size < 512)
        return PbrKind::Unknown;

    // grldr.pbr sector 0 carries "No GRLDR" in its message area; check before
    // the OEM-name heuristics so its template strings cannot cause a misread.
    if (containsAscii(sector, 0x1FE, "No GRLDR"))
        return PbrKind::Grub4dos;

    if (oemIs(sector, "NTFS"))
        return PbrKind::Ntfs;
    if (oemIs(sector, "EXFAT"))
        return PbrKind::ExFat;
    if (oemIs(sector, "MSDOS") || oemIs(sector, "MSWIN") || oemIs(sector, "mkdosfs") ||
        oemIs(sector, "MSDOS5")) {
        // FAT family: distinguish by total-sectors fields + FAT size.
        uint16_t total16 = uint16_t(sector[0x13]) | uint16_t(sector[0x14]) << 8;
        uint32_t total32 = uint32_t(sector[0x20]) | uint32_t(sector[0x21]) << 8 |
                           uint32_t(sector[0x22]) << 16 | uint32_t(sector[0x23]) << 24;
        uint32_t fatSz16 = uint32_t(sector[0x16]) | uint32_t(sector[0x17]) << 8;
        uint32_t fatSz32 = uint32_t(sector[0x24]) | uint32_t(sector[0x25]) << 8 |
                           uint32_t(sector[0x26]) << 16 | uint32_t(sector[0x27]) << 24;
        if (fatSz32 && !fatSz16)
            return PbrKind::Fat32;
        if (total16 || total32) {
            uint32_t total = total16 ? total16 : total32;
            // FAT12 max cluster count -> rough 1MB..4MB heuristic; classify by size.
            if (total <= 8192)
                return PbrKind::Fat12;
            return PbrKind::Fat16;
        }
        return PbrKind::Fat16;
    }
    if (std::memcmp(sector + 0x03, "GRUB", 4) == 0)
        return PbrKind::Grub4dos;
    return PbrKind::Unknown;
}

bool patchNtfsBpb(uint8_t* sector, size_t size, uint64_t hiddenSectors,
                  uint64_t totalSectors)
{
    if (size < 512 || !hasSignature(sector, size) || !oemIs(sector, "NTFS"))
        return false;
    putLe64(sector + 0x1C, hiddenSectors);
    putLe64(sector + 0x28, totalSectors);
    return true;
}

bool patchFatBpb(uint8_t* sector, size_t size, uint32_t hiddenSectors,
                 uint32_t totalSectors)
{
    if (size < 512 || !hasSignature(sector, size))
        return false;
    putLe32(sector + 0x1C, hiddenSectors);
    putLe32(sector + 0x20, totalSectors);
    if (totalSectors <= 0xFFFE)
        putLe16(sector + 0x13, uint16_t(totalSectors));
    else
        putLe16(sector + 0x13, 0); // FAT convention: total32 is authoritative
    return true;
}

bool patchExFatBpb(uint8_t* sector, size_t size, uint64_t hiddenSectors,
                   uint64_t totalSectors)
{
    if (size < 512 || !hasSignature(sector, size) || !oemIs(sector, "EXFAT"))
        return false;
    putLe64(sector + 0x40, hiddenSectors);
    putLe64(sector + 0x48, totalSectors);
    return true;
}

} // namespace bootroll
