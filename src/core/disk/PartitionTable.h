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

} // namespace bootroll
