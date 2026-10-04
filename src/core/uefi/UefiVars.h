#pragma once
// UEFI boot entry (Boot####) logic: EFI_LOAD_OPTION pack/parse, BootOrder
// codec, variable naming and a portable text backup format. Pure byte/text
// handling - no OS calls here (see platform/IUefiVars.h for the access seam).
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bootroll {

// One firmware boot entry ("Boot%04X" variable).
struct BootEntry {
    uint16_t number = 0;      // n in Boot####
    uint32_t attrs = 0;       // EFI_LOAD_OPTION attributes (bit 0 = ACTIVE)
    std::string desc;         // description (UTF-8)
    std::string path;         // file path from the device path (UTF-8, '\'-separated)
    std::vector<uint8_t> raw; // full raw variable payload (optional data kept)

    // MEDIA_HARDDRIVE_DP node (type 04, subType 03), when present: locates the
    // partition the file lives on (BIOS/UEFI Boot#### entries for disks).
    bool hasHdNode = false;
    uint32_t hdPartition = 0;         // 1-based partition number
    uint64_t hdPartStart = 0;         // partition start (LBA)
    uint64_t hdPartSize = 0;          // partition size (in LBA units)
    std::vector<uint8_t> hdSignature; // 16 bytes (MBR disk sig zero-padded / GPT GUID)
    uint8_t hdSignatureType = 0;      // 0 = none, 1 = MBR, 2 = GPT GUID

    // First device path node (UEFI spec "Device Path Protocol" type bytes:
    // 0x01 hardware, 0x02 ACPI, 0x03 messaging, 0x04 media, 0x05 BBS, 0x7F end).
    bool hasDp = false;
    uint8_t firstDpType = 0;
    uint8_t firstDpSubType = 0;
};

// Parameters of the MEDIA_HARDDRIVE_DP node to embed before the file path.
struct HdPathSpec {
    uint32_t partition = 0;
    uint64_t startLba = 0;
    uint64_t sizeLba = 0;
    std::vector<uint8_t> signature; // 16 bytes; shorter input is zero-padded
    uint8_t signatureType = 0;      // 0 = none, 1 = MBR, 2 = GPT GUID
};

// Parse an EFI_LOAD_OPTION payload. Returns false only when the payload is too
// short to hold the 6-byte header; everything else degrades gracefully
// (truncated description / no device path).
bool parseLoadOption(const std::vector<uint8_t>& raw, BootEntry* out);

// Build an EFI_LOAD_OPTION payload. The path is normalized ('/' -> '\',
// leading '\' added). attrs bit 0 marks the entry ACTIVE, bit 4 HIDDEN.
// File-path-only form (device path = one MEDIA_FILEPATH_DP node).
std::vector<uint8_t> packLoadOption(uint32_t attrs, const std::string& descUtf8,
                                    const std::string& filePathUtf8);
// Full form: optional MEDIA_HARDDRIVE_DP node (hd != nullptr) before the file
// path node - the device path firmware uses to boot from a disk partition.
std::vector<uint8_t> packLoadOptionFull(uint32_t attrs, const std::string& descUtf8,
                                        const HdPathSpec* hd,
                                        const std::string& filePathUtf8);

// Documented EFI device path type enumeration (UEFI spec "Device Path
// Protocol"); used to label boot entries in the UI. Spec terms are kept in
// English on purpose.
std::string devicePathTypeLabel(uint8_t type, uint8_t subType);

std::vector<uint16_t> decodeBootOrder(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> encodeBootOrder(const std::vector<uint16_t>& order);

std::string bootVarName(uint16_t n); // n -> "Boot%04X"
bool parseBootVarName(const std::string& name, uint16_t* n);

// Smallest boot number not present in the used set.
uint16_t nextFreeBootNumber(const std::set<uint16_t>& used);

// Snapshot of the UEFI boot configuration (automatic safety copy + export).
struct UefiBackup {
    bool hasTimeout = false;
    uint16_t timeout = 0;
    bool hasBootNext = false;
    uint16_t bootNext = 0;
    std::vector<uint16_t> bootOrder;
    std::map<uint16_t, std::vector<uint8_t>> entries; // Boot#### raw payloads
};

std::string writeUefiBackupText(const UefiBackup& b);
// Strict parser: any unknown line, bad hex or malformed value fails the whole
// file (a half-restored NVRAM would be far worse than a rejected backup).
bool parseUefiBackupText(const std::string& text, UefiBackup* out);

} // namespace bootroll
