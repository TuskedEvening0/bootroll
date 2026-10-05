#include "ui/UefiScreen.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/util/LocalTime.h"
#include "core/disk/DiskInfo.h"
#include "core/uefi/UefiVars.h"
#include "imgui.h"
#include "platform/IUefiVars.h"
#include "ui/EspFileDialog.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bootroll {

namespace {

constexpr int kMaxScanNumber = 0x3FF; // Boot0000..Boot03FF
constexpr int kMaxMissRun = 256;      // stop scanning after this many misses in a row
constexpr uint32_t kVarAttrs = 0x7;   // non-volatile | boot-service | runtime

const ImVec4 kColValue = ImVec4(0.6f, 0.85f, 1.0f, 1.0f);
const ImVec4 kColError = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
const ImVec4 kColOk = ImVec4(0.5f, 0.8f, 0.5f, 1.0f);
const ImVec4 kColWarn = ImVec4(1.0f, 0.85f, 0.3f, 1.0f);

// GPT type GUID of an EFI System Partition.
constexpr const char* kEspTypeGuid = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";

bool isEspPartition(const PartitionInfo& p)
{
    std::string g = p.gptTypeGuid;
    for (char& c : g) {
        c = (char)std::tolower((unsigned char)c);
    }
    return g == kEspTypeGuid;
}

// GUID "00112233-4455-6677-8899-AABBCCDDEEFF" -> 16 bytes in the mixed-endian
// layout used inside GPT entries and MEDIA_HARDDRIVE_DP signatures.
bool guidStringToBytes(const std::string& guid, uint8_t out[16])
{
    std::string hex;
    for (const char c : guid) {
        if (c != '-') {
            hex += c;
        }
    }
    if (hex.size() != 32) {
        return false;
    }
    auto nib = [&hex](size_t i) -> int {
        const char c = hex[i];
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        if (nib(i) < 0) {
            return false;
        }
    }
    auto byteAt = [&nib](size_t i) { return uint8_t((nib(i * 2) << 4) | nib(i * 2 + 1)); };
    out[0] = byteAt(3);
    out[1] = byteAt(2);
    out[2] = byteAt(1);
    out[3] = byteAt(0); // Data1 little-endian
    out[4] = byteAt(5);
    out[5] = byteAt(4); // Data2 little-endian
    out[6] = byteAt(7);
    out[7] = byteAt(6); // Data3 little-endian
    for (int i = 8; i < 16; ++i) {
        out[i] = byteAt(size_t(i)); // Data4 sequential
    }
    return true;
}

// Automatic pre-write backups live in <exe dir>/backup (next to bootroll.log).
std::string backupDir(App& app)
{
    const std::string log = app.platform()->logPath();
    const size_t p = log.find_last_of("/\\");
    std::string dir = (p == std::string::npos) ? std::string(".") : log.substr(0, p);
    dir += "/backup";
    return dir;
}

uint16_t rd16(const std::vector<uint8_t>& v)
{
    return uint16_t(v[0] | (v[1] << 8));
}

std::vector<uint8_t> u16Bytes(uint16_t v)
{
    return { uint8_t(v & 0xFF), uint8_t(v >> 8) };
}

struct UefiUi {
    bool attempted = false;  // load() tried at least once
    bool loaded = false;     // variable snapshot valid
    std::string readyError;  // why the UEFI environment is unavailable
    std::vector<BootEntry> entries; // display order = BootOrder + orphans
    bool orderDirty = false; // pending reorder not written yet
    bool hasTimeout = false;
    uint16_t timeout = 0;
    int timeoutBuf = 0;
    bool hasBootNext = false;
    uint16_t bootNext = 0;
    int selected = -1;
    // add/edit form
    bool editOpen = false;
    bool editNew = false;
    uint16_t editNumber = 0;
    char editDesc[128] = {};
    char editPath[512] = {};
    bool editActive = true;
    bool editHidden = false;  // EFI_LOAD_OPTION_HIDDEN (attributes bit 4)
    bool editBootNext = false; // also write BootNext on apply
    int editDeviceType = 0;   // 0 = disk partition (HDD), 1 = file path only
    std::string editDpLabel;  // read-only device path type label (edit mode)
    int editDiskIdx = -1;     // index into App::disks()
    int editPartIdx = -1;     // index into disk.partitions
    EspFileDialog espDialog;
    std::string error; // red status line
    std::string info;  // green status line
};

UefiUi& ui()
{
    static UefiUi s;
    return s;
}

std::set<uint16_t> usedNumbers(const UefiUi& s)
{
    std::set<uint16_t> used;
    for (const BootEntry& e : s.entries) {
        used.insert(e.number);
    }
    return used;
}

// Snapshot of the currently loaded variables (the automatic safety copy).
UefiBackup snapshot(const UefiUi& s)
{
    UefiBackup b;
    b.hasTimeout = s.hasTimeout;
    b.timeout = s.timeout;
    b.hasBootNext = s.hasBootNext;
    b.bootNext = s.bootNext;
    for (const BootEntry& e : s.entries) {
        b.bootOrder.push_back(e.number);
        b.entries[e.number] = e.raw;
    }
    return b;
}

bool writeAutoBackup(App& app, const UefiUi& s, std::string* outPath, std::string* err)
{
    namespace fs = std::filesystem;
    const std::string dir = backupDir(app);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec && !fs::exists(dir)) {
        *err = std::string(T_("Cannot create backup directory: ")) + ec.message();
        return false;
    }
    const std::time_t t = std::time(nullptr);
    const std::tm tmv = bootroll::localTm(t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    const std::string path = dir + "/uefi_" + stamp + ".uefibak";
    std::ofstream f(fs::path(path), std::ios::binary);
    if (!f) {
        *err = std::string(T_("Cannot write backup file: ")) + path;
        return false;
    }
    const std::string text = writeUefiBackupText(snapshot(s));
    f.write(text.data(), std::streamsize(text.size()));
    f.close();
    if (!f) {
        *err = std::string(T_("Cannot write backup file: ")) + path;
        return false;
    }
    *outPath = path;
    return true;
}

bool load(App& app, UefiUi& s)
{
    s.error.clear();
    s.info.clear();
    s.entries.clear();
    s.orderDirty = false;
    s.selected = -1;
    s.editOpen = false;
    if (app.platform()->firmwareType() != FirmwareType::Uefi) {
        app.log("uefi: firmware is not UEFI, page unavailable");
        s.readyError = T_("No UEFI environment on this system (BIOS boot or missing privilege).");
        s.loaded = false;
        return false;
    }
    IUefiVars* uv = app.platform()->uefiVars();
    std::string err;
    if (!uv->ensureReady(&err)) {
        app.log("uefi: not ready: " + err, App::LogLevel::Warn);
        s.readyError = I18n::instance().translate(err);
        s.loaded = false;
        return false;
    }
    std::vector<uint8_t> data;
    uint32_t attrs = 0;

    // A failed read is only tolerable when the variable simply does not exist
    // (absent BootOrder/BootNext/Timeout are legal states); anything else is a
    // systemic failure (privilege, access, firmware) and must be reported -
    // silently showing an empty list made the page look broken.
    std::vector<uint16_t> order;
    if (uv->read("BootOrder", &data, &attrs, &err)) {
        order = decodeBootOrder(data);
        app.log("uefi: BootOrder ok, " + std::to_string(order.size()) + " refs");
    } else if (!isUefiVarAbsent(uv->lastErrorCode())) {
        app.log("uefi: BootOrder read failed: " + err, App::LogLevel::Error);
        s.readyError = I18n::instance().translate(err);
        s.loaded = false;
        return false;
    } else {
        app.log("uefi: BootOrder absent (fresh firmware)");
    }
    s.hasBootNext = false;
    s.bootNext = 0;
    if (uv->read("BootNext", &data, &attrs, &err)) {
        if (data.size() >= 2) {
            s.hasBootNext = true;
            s.bootNext = rd16(data);
        }
    } else if (!isUefiVarAbsent(uv->lastErrorCode())) {
        s.readyError = I18n::instance().translate(err);
        s.loaded = false;
        return false;
    }
    s.hasTimeout = false;
    s.timeout = 0;
    s.timeoutBuf = 0;
    if (uv->read("Timeout", &data, &attrs, &err)) {
        if (data.size() >= 2) {
            s.hasTimeout = true;
            s.timeout = rd16(data);
            s.timeoutBuf = s.timeout;
        }
    } else if (!isUefiVarAbsent(uv->lastErrorCode())) {
        s.readyError = I18n::instance().translate(err);
        s.loaded = false;
        return false;
    }

    std::map<uint16_t, BootEntry> byNumber;
    int miss = 0;
    int missBreak = -1; // first number of the trailing absent run (diagnostics)
    for (int n = 0; n <= kMaxScanNumber; ++n) {
        if (!uv->read(bootVarName(uint16_t(n)), &data, &attrs, &err)) {
            if (!isUefiVarAbsent(uv->lastErrorCode())) {
                // A real failure: with no entries parsed the scan is useless,
                // report it; with entries found keep what we have.
                app.log("uefi: scan aborted at " + bootVarName(uint16_t(n)) +
                        ": " + err, App::LogLevel::Warn);
                if (byNumber.empty()) {
                    s.readyError = I18n::instance().translate(err);
                    s.loaded = false;
                    return false;
                }
                break;
            }
            if (++miss >= kMaxMissRun) {
                missBreak = n;
                break;
            }
            continue;
        }
        miss = 0;
        BootEntry e;
        if (parseLoadOption(data, &e)) {
            e.number = uint16_t(n);
            byNumber[e.number] = e;
        }
    }
    app.log("uefi: scan found " + std::to_string(byNumber.size()) + " entries" +
            (missBreak >= 0
                 ? " (nothing from " + bootVarName(uint16_t(missBreak)) + " on)"
                 : ""));
    // BootOrder may legally reference numbers beyond the scan window; fetch
    // those individually so the list stays complete.
    for (const uint16_t n : order) {
        if (n <= kMaxScanNumber || byNumber.count(n) != 0) {
            continue;
        }
        if (uv->read(bootVarName(n), &data, &attrs, &err)) {
            BootEntry e;
            if (parseLoadOption(data, &e)) {
                e.number = n;
                byNumber[e.number] = e;
            }
        } else if (!isUefiVarAbsent(uv->lastErrorCode())) {
            app.log("uefi: " + bootVarName(n) + " read failed: " + err, App::LogLevel::Warn);
            s.readyError = I18n::instance().translate(err);
            s.loaded = false;
            return false;
        }
    }
    // BootOrder first, then entries it does not reference (by number).
    std::set<uint16_t> seen;
    for (uint16_t n : order) {
        const auto it = byNumber.find(n);
        if (it != byNumber.end()) {
            s.entries.push_back(it->second);
            seen.insert(n);
        }
    }
    for (const auto& entry : byNumber) {
        if (seen.count(entry.first) == 0) {
            s.entries.push_back(entry.second);
        }
    }
    s.loaded = true;
    app.log("uefi: page ready, " + std::to_string(s.entries.size()) + " entries");
    return true;
}

// Common write path: automatic backup + native confirm, then the writes.
// On success the snapshot is reloaded (which also clears orderDirty).
void guardedWrite(App& app, UefiUi& s, const std::string& question,
                  const std::function<bool(IUefiVars*, std::string*)>& action)
{
    IUefiVars* uv = app.platform()->uefiVars();
    std::string err;
    std::string backupPath;
    if (!writeAutoBackup(app, s, &backupPath, &err)) {
        s.error = err;
        return;
    }
    const std::string text = question + "\n\n" + T_("Automatic backup saved to") +
                             ": " + backupPath;
    if (!app.platform()->confirmDialog(T_("Confirm write"), text)) {
        return;
    }
    if (action(uv, &err)) {
        s.info = T_("UEFI variables updated.");
        s.error.clear();
        load(app, s);
    } else {
        s.error = err;
    }
}

// Find the first ESP across all enumerated disks (defaults for "Add entry").
void findEspDefault(App& app, int* diskIdx, int* partIdx)
{
    const std::vector<DiskInfo>& disks = app.disks();
    for (size_t di = 0; di < disks.size(); ++di) {
        for (size_t pi = 0; pi < disks[di].partitions.size(); ++pi) {
            if (isEspPartition(disks[di].partitions[pi])) {
                *diskIdx = int(di);
                *partIdx = int(pi);
                return;
            }
        }
    }
}

// Locate the disk/partition a parsed MEDIA_HARDDRIVE_DP node points to:
// GPT signature (partition GUID) first, then start/size and partition number.
bool matchHdNode(App& app, const BootEntry& e, int* diskIdx, int* partIdx)
{
    const std::vector<DiskInfo>& disks = app.disks();
    int bestDisk = -1;
    int bestPart = -1;
    int bestScore = 0;
    for (size_t di = 0; di < disks.size(); ++di) {
        const DiskInfo& d = disks[di];
        if (d.sectorSize == 0) {
            continue;
        }
        for (size_t pi = 0; pi < d.partitions.size(); ++pi) {
            const PartitionInfo& p = d.partitions[pi];
            int score = 0;
            if (p.offsetBytes / d.sectorSize == e.hdPartStart &&
                p.sizeBytes / d.sectorSize == e.hdPartSize) {
                score += 1;
            }
            if (e.hdSignatureType == 2 && p.style == PartitionStyle::Gpt &&
                e.hdSignature.size() == 16) {
                uint8_t sig[16];
                if (guidStringToBytes(p.gptPartGuid, sig) &&
                    std::memcmp(sig, e.hdSignature.data(), 16) == 0) {
                    score += 4;
                }
            }
            if (e.hdSignatureType == 1 && p.style == PartitionStyle::Mbr &&
                e.hdPartition == p.number) {
                score += 2;
            }
            if (score > bestScore) {
                bestScore = score;
                bestDisk = int(di);
                bestPart = int(pi);
            }
        }
    }
    if (bestScore == 0) {
        return false;
    }
    *diskIdx = bestDisk;
    *partIdx = bestPart;
    return true;
}

void applyEdit(App& app, UefiUi& s)
{
    const std::string desc = s.editDesc;
    const std::string path = s.editPath;
    if (path.empty()) {
        s.error = T_("No boot file specified.");
        s.info.clear();
        return;
    }
    const uint32_t loadAttrs = (s.editActive ? 0x1u : 0u) | (s.editHidden ? 0x4u : 0u);

    // HDD mode embeds a MEDIA_HARDDRIVE_DP node locating the partition.
    HdPathSpec hd {};
    const HdPathSpec* hdPtr = nullptr;
    if (s.editDeviceType == 0) {
        const std::vector<DiskInfo>& disks = app.disks();
        if (s.editDiskIdx < 0 || s.editDiskIdx >= int(disks.size())) {
            s.error = T_("Select a boot disk and partition first.");
            s.info.clear();
            return;
        }
        const DiskInfo& d = disks[size_t(s.editDiskIdx)];
        if (s.editPartIdx < 0 || s.editPartIdx >= int(d.partitions.size())) {
            s.error = T_("Select a boot disk and partition first.");
            s.info.clear();
            return;
        }
        const PartitionInfo& p = d.partitions[size_t(s.editPartIdx)];
        if (d.sectorSize == 0 || p.offsetBytes % d.sectorSize != 0) {
            s.error = T_("The partition start is not sector aligned.");
            s.info.clear();
            return;
        }
        hd.partition = p.number;
        hd.startLba = p.offsetBytes / d.sectorSize;
        hd.sizeLba = p.sizeBytes / d.sectorSize;
        if (p.style == PartitionStyle::Gpt) {
            uint8_t sig[16] = {};
            if (guidStringToBytes(p.gptPartGuid, sig)) {
                hd.signature.assign(sig, sig + 16);
                hd.signatureType = 2;
            }
        } else if (p.style == PartitionStyle::Mbr) {
            std::vector<uint8_t> mbr(d.sectorSize, 0);
            try {
                app.diskAccess()->readSectors(d.number, 0, mbr.data(), mbr.size());
                hd.signature.assign(16, 0);
                std::copy(mbr.begin() + 0x1B8, mbr.begin() + 0x1BC, hd.signature.begin());
                hd.signatureType = 1;
            } catch (const std::exception&) {
                hd.signatureType = 0; // keep the entry valid without a signature
            }
        }
        hdPtr = &hd;
    } else if (!s.editNew) {
        // Editing an existing entry keeps its MEDIA_HARDDRIVE_DP locator even
        // when its partition could not be matched against the current disk
        // list (e.g. the disk is detached): rebuild it from the parsed values
        // instead of silently degrading the entry to a bare file path.
        if (s.selected >= 0 && s.selected < int(s.entries.size())) {
            const BootEntry& orig = s.entries[size_t(s.selected)];
            if (orig.hasHdNode) {
                hd.partition = orig.hdPartition;
                hd.startLba = orig.hdPartStart;
                hd.sizeLba = orig.hdPartSize;
                hd.signature = orig.hdSignature;
                hd.signatureType = orig.hdSignatureType;
                hdPtr = &hd;
            }
        }
    }

    const std::vector<uint8_t> raw = packLoadOptionFull(loadAttrs, desc, hdPtr, path);
    if (s.editNew) {
        const uint16_t num = nextFreeBootNumber(usedNumbers(s));
        const std::string name = bootVarName(num);
        std::vector<uint16_t> order;
        for (const BootEntry& e : s.entries) {
            order.push_back(e.number);
        }
        order.push_back(num); // new entries join the end of BootOrder
        const std::vector<uint8_t> orderBytes = encodeBootOrder(order);
        const bool setNext = s.editBootNext;
        guardedWrite(app, s, T_("Write UEFI boot entry changes to NVRAM?"),
                     [name, raw, orderBytes, num,
                      setNext](IUefiVars* uv, std::string* err) {
                         if (!uv->write(name, raw, kVarAttrs, err)) {
                             return false;
                         }
                         if (!uv->write("BootOrder", orderBytes, kVarAttrs, err)) {
                             return false;
                         }
                         if (setNext) {
                             return uv->write("BootNext", u16Bytes(num), kVarAttrs, err);
                         }
                         return true;
                     });
    } else {
        const uint16_t num = s.editNumber;
        const std::string name = bootVarName(num);
        const bool setNext = s.editBootNext;
        const bool clearNext = !s.editBootNext && s.hasBootNext && s.bootNext == num;
        guardedWrite(app, s, T_("Write UEFI boot entry changes to NVRAM?"),
                     [name, raw, num, setNext,
                      clearNext](IUefiVars* uv, std::string* err) {
                         if (!uv->write(name, raw, kVarAttrs, err)) {
                             return false;
                         }
                         if (setNext) {
                             return uv->write("BootNext", u16Bytes(num), kVarAttrs, err);
                         }
                         if (clearNext) {
                             return uv->remove("BootNext", err);
                         }
                         return true;
                     });
    }
}

// DiskGenius-style add/edit form: title, device type, boot disk/partition,
// boot file (with an ESP file browser), and the attribute checkboxes.
void drawEditForm(App& app, UefiUi& s)
{
    ImGui::Separator();
    ImGui::TextUnformatted(T_(s.editNew ? "Add entry" : "Edit entry"));
    ImGui::SetNextItemWidth(360);
    ImGui::InputText(T_("Menu title"), s.editDesc, sizeof(s.editDesc));

    if (s.editNew) {
        // Add mode: the two forms bootroll can build, both documented
        // MEDIA_DEVICE_PATH (0x04) entries in the EFI device path spec.
        // T_() yields c_str() of a temporary std::string: keep the strings
        // alive for the frame, or the Combo reads freed heap memory and the
        // preview flickers between the translation and reused bytes.
        const std::string typeHdd =
            T_("Media Device (0x04) - Disk partition boot (HDD)");
        const std::string typeFile = T_("Media Device (0x04) - File path only");
        const char* deviceTypes[] = { typeHdd.c_str(), typeFile.c_str() };
        ImGui::SetNextItemWidth(360);
        if (ImGui::Combo(T_("Device type"), &s.editDeviceType, deviceTypes, 2)) {
            s.error.clear();
        }
    } else {
        // Edit mode: the device path type belongs to the documented EFI device
        // path enumeration (0x01 hardware, 0x02 ACPI, 0x03 messaging, 0x04
        // media, 0x05 BBS) and is fixed by what the firmware created.
        ImGui::TextUnformatted(T_("Device type"));
        ImGui::SameLine();
        ImGui::TextUnformatted(s.editDpLabel.c_str());
        ImGui::TextDisabled(T_("Locked by the firmware device path; cannot be changed."));
    }

    const std::vector<DiskInfo>& disks = app.disks();
    bool partChosen = false;
    if (s.editDeviceType == 0) {
        std::string diskJoined;
        for (const DiskInfo& d : disks) {
            std::string label = diskShortName(d);
            if (!d.model.empty()) {
                label += " - " + d.model;
            }
            label += " (" + formatSize(d.totalBytes) + ")";
            diskJoined += label;
            diskJoined += '\0';
        }
        diskJoined += '\0';
        ImGui::SetNextItemWidth(360);
        if (ImGui::Combo(T_("Boot disk"), &s.editDiskIdx,
                         diskJoined.empty() ? "\0" : diskJoined.c_str())) {
            s.editPartIdx = -1;
        }
        if (s.editDiskIdx >= 0 && s.editDiskIdx < int(disks.size())) {
            const DiskInfo& d = disks[size_t(s.editDiskIdx)];
            std::string partJoined;
            for (const PartitionInfo& p : d.partitions) {
                std::string label = std::string(T_("Partition")) + " " +
                                    std::to_string(p.number);
                if (isEspPartition(p)) {
                    label += " (ESP)";
                } else if (p.style == PartitionStyle::Mbr && p.typeCode != 0) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "0x%02X", p.typeCode);
                    label += std::string(" (") + hex + ")";
                }
                if (!p.fsName.empty()) {
                    label += " - " + p.fsName;
                }
                if (!p.driveLetter.empty()) {
                    label += " (" + p.driveLetter + ")";
                }
                label += " - " + formatSize(p.sizeBytes);
                partJoined += label;
                partJoined += '\0';
            }
            partJoined += '\0';
            ImGui::SetNextItemWidth(360);
            ImGui::Combo(T_("Boot partition"), &s.editPartIdx,
                         d.partitions.empty() ? "\0" : partJoined.c_str());
            partChosen = s.editPartIdx >= 0 &&
                         s.editPartIdx < int(d.partitions.size());
        } else {
            ImGui::TextColored(kColWarn, "%s",
                               T_("No boot disk/partition selected."));
        }
    }

    ImGui::SetNextItemWidth(480);
    ImGui::InputText(T_("Boot file"), s.editPath, sizeof(s.editPath));
    if (s.editDeviceType == 0) {
        ImGui::SameLine();
        ImGui::BeginDisabled(!partChosen);
        if (ImGui::Button(T_("Browse...")) && partChosen) {
            s.espDialog.open(app, s.editDiskIdx, s.editPartIdx);
        }
        ImGui::EndDisabled();
    } else {
        // File-path-only form: the browser just helps locate the file; only
        // the path is stored, no MEDIA_HARDDRIVE_DP locator node is written.
        ImGui::SameLine();
        ImGui::BeginDisabled(disks.empty());
        if (ImGui::Button(T_("Browse..."))) {
            int startDisk = 0;
            int startPart = 0;
            findEspDefault(app, &startDisk, &startPart);
            if (startDisk < 0) { // no ESP anywhere: start from disk 0
                startDisk = 0;
                startPart = 0;
            }
            s.espDialog.open(app, startDisk, startPart);
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            T_("Executable path on the ESP, e.g. \\EFI\\Microsoft\\Boot\\bootmgfw.efi"));
    }

    ImGui::TextUnformatted(T_("Attributes"));
    ImGui::Checkbox(T_("Active"), &s.editActive);
    ImGui::SameLine();
    ImGui::Checkbox(T_("Hidden"), &s.editHidden);
    ImGui::SameLine();
    ImGui::Checkbox(T_("Set as next boot (BootNext)"), &s.editBootNext);

    if (ImGui::Button(T_("Apply"))) {
        applyEdit(app, s);
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Cancel"))) {
        s.editOpen = false;
        s.espDialog.close();
    }

    if (s.espDialog.isOpen()) {
        std::string picked;
        if (s.espDialog.draw(app, &picked)) {
            std::snprintf(s.editPath, sizeof(s.editPath), "%s", picked.c_str());
        }
    }
}

void drawEntryTable(UefiUi& s)
{
    if (!ImGui::BeginTable("##uefientries", 5, ImGuiTableFlags_Borders |
                                                  ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_Resizable |
                                                  ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn(T_("Order"), ImGuiTableColumnFlags_WidthFixed, 52);
    ImGui::TableSetupColumn("Boot####", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn(T_("Description"));
    ImGui::TableSetupColumn(T_("Path"));
    ImGui::TableSetupColumn(T_("Flags"), ImGuiTableColumnFlags_WidthFixed, 140);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < s.entries.size(); ++i) {
        const BootEntry& e = s.entries[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        char orderTxt[32];
        std::snprintf(orderTxt, sizeof(orderTxt), "%zu", i);
        if (ImGui::Selectable(orderTxt, s.selected == int(i),
                              ImGuiSelectableFlags_SpanAllColumns)) {
            s.selected = int(i);
        }
        const std::string name = bootVarName(e.number);
        ImGui::TableSetColumnIndex(1);
        if (s.hasBootNext && e.number == s.bootNext) {
            ImGui::TextColored(kColValue, "%s", name.c_str());
        } else {
            ImGui::TextUnformatted(name.c_str());
        }
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(e.desc.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(e.path.c_str());
        ImGui::TableSetColumnIndex(4);
        std::string flags = (e.attrs & 1) ? T_("Active") : "-";
        if ((e.attrs & 0x4) != 0) {
            flags += " ";
            flags += T_("Hidden");
        }
        char attrsHex[16];
        std::snprintf(attrsHex, sizeof(attrsHex), " %08X", e.attrs);
        flags += attrsHex;
        ImGui::TextUnformatted(flags.c_str());
    }
    ImGui::EndTable();
}

void drawBodyImpl(App& app)
{
    UefiUi& s = ui();
    if (!s.attempted) {
        s.attempted = true;
        load(app, s);
    }

    if (!s.loaded) {
        ImGui::TextColored(kColError, "%s", s.readyError.c_str());
        if (ImGui::Button(T_("Refresh"))) {
            load(app, s);
        }
        if (app.platform()->firmwareType() == FirmwareType::Uefi &&
            !app.platform()->isElevated()) {
            ImGui::SameLine();
            if (ImGui::Button(T_("Restart as Administrator"))) {
                if (app.platform()->restartElevated("")) {
                    app.requestExit(); // elevated instance takes over
                } else {
                    s.readyError = T_("Restart as Administrator was declined or failed.");
                }
            }
        }
        return;
    }

    if (!s.error.empty()) {
        ImGui::TextColored(kColError, "%s", s.error.c_str());
    }
    if (!s.info.empty()) {
        ImGui::TextColored(kColOk, "%s", s.info.c_str());
    }

    drawEntryTable(s);
    if (s.entries.empty()) {
        ImGui::TextDisabled(T_("No UEFI boot entries found."));
    }

    const bool haveSel = s.selected >= 0 && s.selected < int(s.entries.size());

    if (ImGui::Button(T_("Refresh"))) {
        load(app, s);
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Add entry"))) {
        s.editOpen = true;
        s.editNew = true;
        s.editNumber = 0;
        s.editDesc[0] = '\0';
        s.editPath[0] = '\0';
        s.editActive = true;
        s.editHidden = false;
        s.editBootNext = false;
        s.editDeviceType = 0;
        s.editDpLabel.clear();
        s.editDiskIdx = -1;
        s.editPartIdx = -1;
        findEspDefault(app, &s.editDiskIdx, &s.editPartIdx);
        if (s.editDiskIdx < 0) { // no ESP anywhere: start from disk 0
            s.editDiskIdx = 0;
            s.editPartIdx = app.disks().empty() || app.disks()[0].partitions.empty()
                                ? -1
                                : 0;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveSel);
    if (ImGui::Button(T_("Edit entry"))) {
        const BootEntry& e = s.entries[size_t(s.selected)];
        s.editOpen = true;
        s.editNew = false;
        s.editNumber = e.number;
        std::snprintf(s.editDesc, sizeof(s.editDesc), "%s", e.desc.c_str());
        std::snprintf(s.editPath, sizeof(s.editPath), "%s", e.path.c_str());
        s.editActive = (e.attrs & 1) != 0;
        s.editHidden = (e.attrs & 0x4) != 0;
        s.editBootNext = s.hasBootNext && s.bootNext == e.number;
        if (e.hasHdNode && matchHdNode(app, e, &s.editDiskIdx, &s.editPartIdx)) {
            s.editDeviceType = 0;
        } else {
            s.editDeviceType = 1;
        }
        s.editDpLabel = e.hasDp
                            ? devicePathTypeLabel(e.firstDpType, e.firstDpSubType)
                            : std::string(T_("No device path in this entry."));
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Delete entry"))) {
        const BootEntry& e = s.entries[size_t(s.selected)];
        const uint16_t num = e.number;
        const std::string name = bootVarName(num);
        const std::string q = std::string(T_("Delete this UEFI boot entry from NVRAM?")) +
                              "\n\n" + name + "  " + e.desc;
        guardedWrite(app, s, q, [name, num](IUefiVars* uv, std::string* err) {
            if (!uv->remove(name, err)) {
                return false;
            }
            // Keep BootOrder consistent with the remaining entries.
            std::vector<uint8_t> data;
            uint32_t attrs = 0;
            std::string readErr;
            std::vector<uint16_t> order;
            if (uv->read("BootOrder", &data, &attrs, &readErr)) {
                order = decodeBootOrder(data);
            }
            std::erase(order, num);
            return uv->write("BootOrder", encodeBootOrder(order), kVarAttrs, err);
        });
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!haveSel || s.selected == 0);
    if (ImGui::Button(T_("Move Up"))) {
        std::swap(s.entries[size_t(s.selected - 1)], s.entries[size_t(s.selected)]);
        s.selected--;
        s.orderDirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveSel || s.selected == int(s.entries.size()) - 1);
    if (ImGui::Button(T_("Move Down"))) {
        std::swap(s.entries[size_t(s.selected)], s.entries[size_t(s.selected + 1)]);
        s.selected++;
        s.orderDirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.orderDirty);
    if (ImGui::Button(T_("Save Order"))) {
        std::vector<uint16_t> order;
        for (const BootEntry& e : s.entries) {
            order.push_back(e.number);
        }
        const std::vector<uint8_t> orderBytes = encodeBootOrder(order);
        guardedWrite(app, s, T_("Write UEFI boot entry changes to NVRAM?"),
                     [orderBytes](IUefiVars* uv, std::string* err) {
                         return uv->write("BootOrder", orderBytes, kVarAttrs, err);
                     });
    }
    ImGui::EndDisabled();
    if (s.orderDirty) {
        ImGui::SameLine();
        ImGui::TextColored(kColWarn, "*");
        ImGui::SameLine();
        ImGui::TextUnformatted(T_("Order changed. Press Save Order to write BootOrder."));
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("BootNext:");
    ImGui::SameLine();
    if (s.hasBootNext) {
        const std::string bn = bootVarName(s.bootNext);
        ImGui::TextColored(kColValue, "%s", bn.c_str());
    } else {
        ImGui::TextUnformatted(T_("(none)"));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!haveSel);
    if (ImGui::Button(T_("Set as BootNext"))) {
        const uint16_t num = s.entries[size_t(s.selected)].number;
        const std::vector<uint8_t> bytes = u16Bytes(num);
        guardedWrite(app, s, T_("Write UEFI boot entry changes to NVRAM?"),
                     [bytes](IUefiVars* uv, std::string* err) {
                         return uv->write("BootNext", bytes, kVarAttrs, err);
                     });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.hasBootNext);
    if (ImGui::Button(T_("Clear BootNext"))) {
        guardedWrite(app, s, T_("Write UEFI boot entry changes to NVRAM?"),
                     [](IUefiVars* uv, std::string* err) {
                         return uv->remove("BootNext", err);
                     });
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputInt(T_("Timeout (s)"), &s.timeoutBuf);
    if (s.timeoutBuf < 0) {
        s.timeoutBuf = 0;
    }
    if (s.timeoutBuf > 0xFFFF) {
        s.timeoutBuf = 0xFFFF;
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Save Timeout"))) {
        const std::vector<uint8_t> bytes = u16Bytes(uint16_t(s.timeoutBuf));
        guardedWrite(app, s, T_("Write UEFI boot entry changes to NVRAM?"),
                     [bytes](IUefiVars* uv, std::string* err) {
                         return uv->write("Timeout", bytes, kVarAttrs, err);
                     });
    }

    ImGui::Spacing();
    if (ImGui::Button(T_("Export backup..."))) {
        const std::string filter = std::string(T_("UEFI backup files")) +
                                   "|*.uefibak|" + T_("All files") + "|*.*";
        const std::string path =
            app.platform()->saveFileDialog(T_("Export backup..."), filter, "uefibak");
        if (!path.empty()) {
            std::ofstream f(std::filesystem::path(path), std::ios::binary);
            const std::string text = writeUefiBackupText(snapshot(s));
            f.write(text.data(), std::streamsize(text.size()));
            f.close();
            if (f) {
                s.info = std::string(T_("UEFI backup saved to")) + ": " + path;
                s.error.clear();
            } else {
                s.error = std::string(T_("Cannot write backup file: ")) + path;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Restore backup..."))) {
        const std::string filter = std::string(T_("UEFI backup files")) +
                                   "|*.uefibak|" + T_("All files") + "|*.*";
        const std::string path =
            app.platform()->openFileDialog(T_("Restore backup..."), filter);
        if (!path.empty()) {
            std::ifstream f(std::filesystem::path(path), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
            UefiBackup restore;
            if (f.fail() && text.empty()) {
                s.error = std::string(T_("Cannot open file: ")) + path;
            } else if (!parseUefiBackupText(text, &restore)) {
                s.error = T_("Invalid UEFI backup file.");
                s.info.clear();
            } else {
                guardedWrite(app, s,
                             T_("Restore this UEFI backup? Existing boot entries will be replaced."),
                             [restore](IUefiVars* uv, std::string* err) {
                                 // Delete entries the backup does not contain.
                                 std::set<uint16_t> keep;
                                 for (const auto& entry : restore.entries) {
                                     keep.insert(entry.first);
                                 }
                                 for (const BootEntry& e : ui().entries) {
                                     if (keep.count(e.number) != 0) {
                                         continue;
                                     }
                                     std::string ignored;
                                     uv->remove(bootVarName(e.number), &ignored);
                                 }
                                 for (const auto& entry : restore.entries) {
                                     if (!uv->write(bootVarName(entry.first),
                                                    entry.second, kVarAttrs, err)) {
                                         return false;
                                     }
                                 }
                                 if (!uv->write("BootOrder",
                                                encodeBootOrder(restore.bootOrder),
                                                kVarAttrs, err)) {
                                     return false;
                                 }
                                 if (restore.hasBootNext &&
                                     !uv->write("BootNext", u16Bytes(restore.bootNext),
                                                kVarAttrs, err)) {
                                     return false;
                                 }
                                 if (restore.hasTimeout &&
                                     !uv->write("Timeout", u16Bytes(restore.timeout),
                                                kVarAttrs, err)) {
                                     return false;
                                 }
                                 return true;
                             });
            }
        }
    }

    if (s.editOpen) {
        drawEditForm(app, s);
    }
}

} // namespace

void UefiScreen::drawBody(App& app)
{
    drawBodyImpl(app);
}

} // namespace bootroll
