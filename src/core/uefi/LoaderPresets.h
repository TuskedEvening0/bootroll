#pragma once
// M11 quick-add: well-known ESP loader detection. Given a path-existence probe
// for one ESP, decide which known loaders (systemd-boot, Limine, rEFInd) can
// get a one-click Boot#### entry. Pure logic - no OS calls here; the caller
// supplies the probe (FatVolume::findEntry over a read-only ESP walk).
#include <functional>
#include <string>
#include <vector>

namespace bootroll {

// A loader detected on one ESP.
struct LoaderHit {
    std::string id;          // stable preset id ("systemd-boot", "limine", "refind")
    std::string description; // fixed entry description for the Boot#### variable
    std::string efiPath;     // loader executable, '\'-separated ESP-relative path
    std::string note;        // English evidence tag; "" for a primary-path match
};

// Detect known loaders on one ESP, best evidence first, at most one hit per
// preset. `exists` receives a '\'-separated ESP-relative path and answers
// whether a file lives there (case-insensitive lookup is the probe's job).
//
// Ambiguity rules (ROADMAP M11): \EFI\BOOT\BOOTX64.EFI could be anything, so
// it is only reported for Limine when limine.conf/limine.cfg confirms it, and
// for systemd-boot only as a last-resort fallback when nothing else matched
// (the hit's note asks the user to verify the description).
std::vector<LoaderHit> detectEspLoaders(
    const std::function<bool(const std::string&)>& exists);

} // namespace bootroll
