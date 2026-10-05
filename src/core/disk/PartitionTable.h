#pragma once
// MBR partition table (DPT) parsing from a raw 512-byte boot sector.
// Editing is intentionally limited to flags the UI exposes (active flag);
// creating partitions is left to disk management tools.
#include <cstdint>
#include <string>
#include <vector>

namespace bootroll {

struct MbrEntry {
    bool active = false;    // status byte 0x80
    bool empty = true;      // type == 0 and no sectors
    uint8_t type = 0;       // partition type byte
    uint32_t beginLba = 0;  // start LBA (relative to the disk)
    uint32_t sectorCount = 0;
};

struct MbrTable {
    bool bootSignature = false;   // 0x55AA at 0x1FE
    uint32_t diskSignature = 0;   // 4 bytes at 0x1B8 (little endian)
    std::vector<MbrEntry> entries; // always 4 entries
};

// One GPT partition entry (128+ bytes, little-endian on disk).
struct GptEntry {
    std::string typeGuid;    // lowercase partition-type GUID string
    std::string partGuid;    // lowercase unique partition GUID string
    uint64_t firstLba = 0;
    uint64_t lastLba = 0;    // inclusive
    uint64_t attributes = 0;
    std::string name;        // UTF-8 (decoded from UTF-16LE)
};

// Parsed GPT header (LBA 1 / backup header).
struct GptHeader {
    bool valid = false;          // signature + header CRC32 ok
    uint32_t headerSize = 0;
    uint64_t currentLba = 0;
    uint64_t backupLba = 0;
    uint64_t firstUsableLba = 0;
    uint64_t lastUsableLba = 0;
    uint64_t entryArrayLba = 0;  // LBA of the partition entry array
    uint32_t entryCount = 0;
    uint32_t entrySize = 0;      // usually 128
    uint32_t entryCrc = 0;       // CRC32 of the entry array
};

// Parse a 512-byte GPT header sector ("EFI PART" at offset 0). Validates the
// header CRC32. Never throws; an invalid sector yields valid=false.
GptHeader parseGptHeader(const uint8_t* sector, size_t size);

// Decode GPT entries from a raw entry array. Validates the array CRC32
// against header.entryCrc; a mismatch yields an empty vector.
std::vector<GptEntry> parseGptEntries(const GptHeader& header,
                                      const uint8_t* bytes, size_t size);

// IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320) - GPT and general use.
uint32_t crc32(const uint8_t* data, size_t size);

// Well-known MBR partition type labels ("" when unknown).
std::string_view mbrPartitionTypeLabel(uint8_t type);

// Parse a 512-byte MBR sector. Never throws; a short/invalid sector yields
// bootSignature=false with zeroed entries.
MbrTable parseMbr(const uint8_t* sector, size_t size);

// Set/clear the active flag of one entry (0-based) in a 512-byte sector.
// Returns false when the index is out of range.
bool setMbrActiveEntry(uint8_t* sector, size_t size, int index, bool active);

// Set one entry active and clear the flag on every other entry (an MBR
// table marks at most one partition active). Returns false out of range.
bool setMbrActiveExclusive(uint8_t* sector, size_t size, int index);

// Set the type byte of one entry (0-based) in a 512-byte sector.
bool setMbrEntryType(uint8_t* sector, size_t size, int index, uint8_t type);

// Clear one entry completely: type, active flag, start LBA and count.
bool clearMbrEntry(uint8_t* sector, size_t size, int index);

// Hide <-> unhide type mapping used by partition tools: 0x07<->0x17,
// 0x0B<->0x1B, 0x0C<->0x1C, 0x06<->0x16, 0x0E<->0x1E, 0x01<->0x11,
// 0x04<->0x14. Returns 0 when the type has no hidden/visible counterpart.
uint8_t mbrHiddenTypeOf(uint8_t type);
uint8_t mbrVisibleTypeOf(uint8_t type);

// GPT partition type GUID of an EFI System Partition (lowercase string form).
inline constexpr std::string_view kGptEspTypeGuid = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";

} // namespace bootroll
