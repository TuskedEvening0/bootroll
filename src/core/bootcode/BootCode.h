#pragma once
// Boot-code registry: MBR types bootroll can install, detection of what is
// currently on a sector, and the install transform (code + preserve signature
// and partition table).
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bootroll {

// Installed MBR flavors bootroll recognizes.
enum class MbrKind {
    Unknown,
    NotMbr,     // no 0x55AA signature
    Nt6,        // NT6 family (Vista+ semantics: INT13h extensions + DAP)
    Nt5,        // NT5 family (XP-era CHS-style)
    Grub4dos,   // grldr.mbr stage1
    Wee,        // WEE inline boot manager
    Syslinux,   // syslinux/gptmbr
    Plop,       // Plop Boot Manager
    Reactive,   // MBR with active flag but unknown code
};

// Installable MBR types (order shown in the UI dropdown). Plop is detected
// (MbrKind::Plop) but not installable: its license does not allow bundling.
enum class MbrInstallType {
    Nt6,      // bootroll's own NT6-style code
    Grub4dos, // grldr.mbr (occupies the first 16 sectors)
    Wee,      // wee63.mbr (occupies the first 63 sectors)
    Syslinux, // syslinux mbr.bin (440-byte code area)
};

std::string_view mbrKindLabel(MbrKind k);

// Heuristic detection from a raw 512-byte sector.
MbrKind detectMbrKind(const uint8_t* sector, size_t size);

// Descriptor of the embedded install payload for a type.
struct MbrInstallInfo {
    const uint8_t* blob;
    size_t blobSize;
    bool multiSector; // true: installs into sectors beyond 0 as well
};

bool mbrInstallInfo(MbrInstallType t, MbrInstallInfo* out);

// Builds the exact payload to write for a target.
//  - single-sector installs (Nt6 / Syslinux): the target's bytes [440, 512)
//    (disk signature, partition table, boot signature) are preserved;
//  - multi-sector installs (Grub4dos / Wee): sector 0 keeps the target's
//    disk signature + partition table area [0x1B8, 0x1FE); later sectors come
//    from the blob verbatim (Wee is zero-padded to a full 63-sector track).
// targetSector0 must be >= 512 bytes. Returns false on invalid input.
bool stageMbrInstall(MbrInstallType t, const uint8_t* targetSector0, size_t size,
                     std::vector<uint8_t>* staged);

// Raw embedded blobs for the Grub4DOS screen (GRLDR file writing, PBR
// install) and the WEE variant. Return nullptr/0 for missing blobs.
const uint8_t* grub4dosGrldrBlob(size_t* size);
const uint8_t* grub4dosPbrBlob(size_t* size);
const uint8_t* wee63MbrBlob(size_t* size);

// Apply installable code onto a target 512-byte sector: bytes [0, 440) come
// from the blob; the target's disk signature, partition table and boot
// signature (bytes [440, 512)) are preserved. size must be >= 512.
// Only valid for single-sector installs; returns false for Grub4dos / Wee
// (use stageMbrInstall instead). Returns false and leaves the sector
// untouched on invalid input.
bool applyMbrCode(uint8_t* sector, size_t size, MbrInstallType t);

} // namespace bootroll
