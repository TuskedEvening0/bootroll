#include "core/uefi/LoaderPresets.h"

namespace bootroll {

namespace {

// Fixed descriptions follow the loaders' own tooling so firmware-side tools
// (bootctl, limine-entry-tool, ...) recognize the entries they created.
constexpr const char* kSystemdDesc = "Linux Boot Manager"; // bootctl's official description
constexpr const char* kLimineDesc = "Limine";
constexpr const char* kRefindDesc = "rEFInd Boot Manager";

constexpr const char* kSystemdEfi = "\\EFI\\systemd\\systemd-bootx64.efi";
constexpr const char* kLimineEfi = "\\EFI\\LIMINE\\LIMINE.EFI";
constexpr const char* kRefindEfi = "\\EFI\\refind\\refind_x64.efi";
constexpr const char* kFallbackEfi = "\\EFI\\BOOT\\BOOTX64.EFI";

// Limine config markers that turn an ambiguous BOOTX64.EFI into a confirmed
// Limine installation (ROADMAP M11). Root, next to the loader, or next to the
// fallback copy; FatVolume lookups are case-insensitive.
bool hasLimineConfig(const std::function<bool(const std::string&)>& exists)
{
    return exists("\\limine.conf") || exists("\\limine.cfg") ||
           exists("\\EFI\\LIMINE\\LIMINE.CONF") || exists("\\EFI\\LIMINE\\LIMINE.CFG") ||
           exists("\\EFI\\BOOT\\LIMINE.CONF") || exists("\\EFI\\BOOT\\LIMINE.CFG");
}

} // namespace

std::vector<LoaderHit> detectEspLoaders(
    const std::function<bool(const std::string&)>& exists)
{
    std::vector<LoaderHit> hits;
    const bool bootx64 = exists(kFallbackEfi);
    const bool limineCfg = hasLimineConfig(exists);

    if (exists(kSystemdEfi)) {
        hits.push_back({"systemd-boot", kSystemdDesc, kSystemdEfi, ""});
    }
    if (exists(kLimineEfi)) {
        hits.push_back({"limine", kLimineDesc, kLimineEfi, ""});
    } else if (bootx64 && limineCfg) {
        hits.push_back({"limine", kLimineDesc, kFallbackEfi,
                        "confirmed by limine.conf/limine.cfg"});
    }
    if (exists(kRefindEfi)) {
        hits.push_back({"refind", kRefindDesc, kRefindEfi, ""});
    }

    // BOOTX64.EFI as a last-resort guess: only when nothing else identified
    // the ESP and no Limine config evidence exists (that case is handled
    // above). Any loader may be named BOOTX64.EFI - ask the user to verify.
    if (hits.empty() && bootx64 && !limineCfg) {
        hits.push_back({"systemd-boot", kSystemdDesc, kFallbackEfi,
                        "fallback path - verify the description"});
    }
    return hits;
}

} // namespace bootroll
