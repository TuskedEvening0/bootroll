#pragma once
// Platform abstraction: the single seam between portable app/core code and the OS.
// One implementation per OS (win/ now, linux/ in M8).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/disk/DiskInfo.h"
#include "core/disk/IDiskAccess.h"

namespace bootroll {

class IUefiVars; // UEFI variable access seam (platform/win/UefiWin.cpp)

enum class FirmwareType {
    Unknown,
    Bios,
    Uefi,
};

// A mounted volume (logical drive / mount point) seen by the OS.
struct VolumeInfo {
    std::string driveLetter;   // "C:" or "" (letter-less, e.g. ESP)
    std::string label;         // volume label (UTF-8), "" if none
    std::string fsName;        // "NTFS", "FAT32", ... "" if unknown
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    uint32_t diskNumber = 0xFFFFFFFF; // backing disk index
    uint32_t partitionNumber = 0xFFFFFFFF;
    bool isEsp = false;               // EFI System Partition (best effort)
};

class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual std::unique_ptr<IDiskAccess> createDiskAccess() = 0;

    virtual std::vector<VolumeInfo> enumerateVolumes() = 0;

    // Read the first sector of a mounted volume ("C:"), e.g. to grab a
    // reference partition boot record. Returns false and fills *error.
    virtual bool readVolumeFirstSector(const std::string& driveLetter,
                                       std::vector<uint8_t>* out,
                                       std::string* error) = 0;

    // Native file dialogs. filter uses '|' separators:
    //   "Boot sector files|*.bin|All files|*.*"
    // Returns "" when the user cancels.
    virtual std::string openFileDialog(const std::string& title,
                                       const std::string& filter) = 0;
    virtual std::string saveFileDialog(const std::string& title,
                                       const std::string& filter,
                                       const std::string& defaultExt) = 0;

    // Native modal confirmation dialog owned by the main window (drawn by the
    // OS above it, not by ImGui). Returns true when the user confirms.
    virtual bool confirmDialog(const std::string& title,
                               const std::string& text) = 0;

    virtual FirmwareType firmwareType() = 0;

    // UEFI variable access (Boot####/BootOrder/BootNext/Timeout). The returned
    // object is owned by the platform implementation and lives as long as it.
    virtual IUefiVars* uefiVars() = 0;

    // DPI scale of the primary display at startup (96 dpi = 1.0x).
    virtual float dpiScale() const = 0;

    // Path of the running system's BCD ("C:\Boot\BCD" / ESP BCD), "" if not found.
    virtual std::string systemBcdPath() = 0;

    // Preferred CJK-capable system font paths, best first ("" entries skipped).
    virtual std::vector<std::string> candidateFontPaths() = 0;

    virtual bool isElevated() = 0;
    // Restart this process elevated (UAC/pkexec). Returns false if declined.
    virtual bool restartElevated(const std::string& args) = 0;

    // UI-facing elevation strings as i18n msgids. The wording is
    // platform-specific: Windows has "Administrator" (UAC), Linux has "root".
    virtual const char* elevateActionMsgId() const = 0;
    virtual const char* elevateDeclinedMsgId() const = 0;

    // Full path of the per-exe settings file (bootroll.ini next to the exe).
    virtual std::string iniPath() = 0;

    // Path of a writable log file (bootroll.log next to the exe).
    virtual std::string logPath() = 0;
};

// Factory implemented by the platform layer.
std::unique_ptr<IPlatform> createPlatform();

} // namespace bootroll
