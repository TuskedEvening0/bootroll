#include "ui/Grub4dosScreen.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/util/LocalTime.h"
#include "core/bootcode/BootCode.h"
#include "core/bootcode/PbrCode.h"
#include "ui/widgets/DiskPicker.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace bootroll {

namespace {

constexpr size_t kSector = 512;

static const ImVec4 kColValue = ImVec4(0.6f, 0.85f, 1.0f, 1.0f);
static const ImVec4 kColError = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
static const ImVec4 kColOk = ImVec4(0.5f, 0.8f, 0.5f, 1.0f);

// Automatic pre-write backups live in <exe dir>/backup (next to bootroll.log).
std::string backupDir(App& app)
{
    const std::string log = app.platform()->logPath();
    const size_t p = log.find_last_of("/\\");
    std::string dir = (p == std::string::npos) ? std::string(".") : log.substr(0, p);
    dir += "/backup";
    return dir;
}

bool writeAutoBackup(App& app, const std::string& tag, const uint8_t* data,
                     size_t size, std::string* outPath, std::string* err)
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
    const std::string path = dir + "/" + tag + "_" + stamp + ".bin";
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        *err = std::string(T_("Cannot write backup file: ")) + path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    f.close();
    if (!f) {
        *err = std::string(T_("Cannot write backup file: ")) + path;
        return false;
    }
    *outPath = path;
    return true;
}

std::string partitionLabel(const PartitionInfo& p)
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), T_("Partition %u  [%s]"), p.number,
                  formatSize(p.sizeBytes).c_str());
    std::string s = buf;
    if (!p.driveLetter.empty()) {
        s += "  " + p.driveLetter;
    }
    if (!p.fsName.empty()) {
        s += "  (" + p.fsName + ")";
    }
    if (!p.label.empty()) {
        s += "  \"" + p.label + "\"";
    }
    return s;
}

struct GrubUi {
    int partIndex = 0;              // index into currentDisk()->partitions
    bool loaded = false;            // cached MBR + PBR sectors valid
    std::string triedKey;           // target signature of the last auto-read attempt
    std::vector<uint8_t> mbrSector; // cached disk sector 0
    std::vector<uint8_t> pbrSector; // cached partition boot sector
    MbrKind mbrKind = MbrKind::Unknown;
    PbrKind pbrKind = PbrKind::Unknown;

    int codeTarget = 0;             // 0 = MBR (grldr.mbr), 1 = PBR (grldr.pbr)

    // menu.lst editor
    char menuText[16384] = {};
    char menuFilePath[1024] = {};

    std::string error;
    std::string info;
};

GrubUi& ui()
{
    static GrubUi s;
    return s;
}

bool readTargets(App& app, GrubUi& st, std::string* err)
{
    const DiskInfo* d = app.currentDisk();
    if (!d) {
        *err = T_("No disk selected.");
        return false;
    }
    st.mbrSector.assign(kSector, 0);
    try {
        app.diskAccess()->readSectors(d->number, 0, st.mbrSector.data(), st.mbrSector.size());
    } catch (const std::exception& e) {
        *err = e.what();
        return false;
    }
    st.mbrKind = detectMbrKind(st.mbrSector.data(), st.mbrSector.size());

    if (d->partitions.empty()) {
        st.pbrSector.clear();
        st.pbrKind = PbrKind::Unknown;
    } else {
        if (st.partIndex < 0 || st.partIndex >= (int)d->partitions.size()) {
            st.partIndex = 0;
        }
        const PartitionInfo& p = d->partitions[st.partIndex];
        st.pbrSector.assign(kSector, 0);
        try {
            app.diskAccess()->readSectors(d->number, p.offsetBytes, st.pbrSector.data(),
                                          st.pbrSector.size());
        } catch (const std::exception& e) {
            *err = e.what();
            return false;
        }
        st.pbrKind = detectPbrKind(st.pbrSector.data(), st.pbrSector.size());
    }
    st.loaded = true;
    return true;
}

bool writeStaged(App& app, GrubUi& st, const std::vector<uint8_t>& bytes, std::string* err)
{
    const DiskInfo* d = app.currentDisk();
    if (!d) {
        *err = T_("No disk selected.");
        return false;
    }
    uint64_t offset = 0;
    if (st.codeTarget == 1) {
        if (d->partitions.empty()) {
            *err = T_("This disk has no partitions.");
            return false;
        }
        offset = d->partitions[st.partIndex].offsetBytes;
    }
    try {
        app.diskAccess()->writeSectors(d->number, offset, bytes.data(), bytes.size());
        app.diskAccess()->flush(d->number);
    } catch (const std::exception& e) {
        *err = e.what();
        return false;
    }
    return true;
}

// Fresh read + automatic backup + native confirm; writes on OK.
void confirmAndWrite(App& app, GrubUi& st, std::vector<uint8_t> staged)
{
    std::string err;
    const DiskInfo* d = app.currentDisk();
    if (!d) {
        st.error = T_("No disk selected.");
        return;
    }
    uint64_t offset = 0;
    std::string tag = "grub_disk" + std::to_string(d->number);
    if (st.codeTarget == 1) {
        if (st.partIndex < 0 || st.partIndex >= (int)d->partitions.size()) {
            st.error = T_("No partition selected.");
            return;
        }
        offset = d->partitions[st.partIndex].offsetBytes;
        tag += "_p" + std::to_string(d->partitions[st.partIndex].number);
    }
    std::vector<uint8_t> backup(staged.size(), 0);
    try {
        app.diskAccess()->readSectors(d->number, offset, backup.data(), backup.size());
    } catch (const std::exception& e) {
        st.error = e.what();
        return;
    }
    std::string bpath;
    if (!writeAutoBackup(app, tag, backup.data(), backup.size(), &bpath, &err)) {
        st.error = err;
        return;
    }
    char head[160];
    std::snprintf(
        head, sizeof(head),
        T_("Install this boot sector? The first %u sectors of the target will be overwritten."),
        (unsigned)(staged.size() / 512));
    const std::string text = std::string(head) + "\n\n" + T_("Automatic backup saved to") +
                             ": " + bpath;
    if (app.platform()->confirmDialog(T_("Confirm write"), text)) {
        if (writeStaged(app, st, staged, &err)) {
            st.info = T_("Boot code installed.");
            st.error.clear();
            st.loaded = false;   // refresh the readout from disk next time
            st.triedKey.clear();
        } else {
            st.error = err;
        }
    }
}

void installBootCode(App& app, GrubUi& st)
{
    std::string err;
    if (st.codeTarget == 0) {
        std::vector<uint8_t> staged;
        if (!stageMbrInstall(MbrInstallType::Grub4dos, st.mbrSector.data(),
                             st.mbrSector.size(), &staged)) {
            st.error = T_("Cannot apply the MBR code to this target.");
            return;
        }
        confirmAndWrite(app, st, std::move(staged));
        return;
    }

    // grldr.pbr: write 11 sectors, keeping the target's BPB so the loader can
    // parse the volume it sits on.
    size_t blobSize = 0;
    const uint8_t* blob = grub4dosPbrBlob(&blobSize);
    if (!blob || blobSize < 512) {
        st.error = T_("Boot code not bundled in this build.");
        return;
    }
    if (!readTargets(app, st, &err)) {
        st.error = err;
        return;
    }
    std::vector<uint8_t> staged(blob, blob + blobSize);
    std::memcpy(staged.data() + 0x0B, st.pbrSector.data() + 0x0B, 0x40 - 0x0B);
    confirmAndWrite(app, st, std::move(staged));
}

void writeGrldrFile(App& app, GrubUi& st)
{
    const DiskInfo* d = app.currentDisk();
    if (!d || st.partIndex < 0 || st.partIndex >= (int)d->partitions.size()) {
        st.error = T_("No partition selected.");
        return;
    }
    const std::string letter = d->partitions[st.partIndex].driveLetter;
    if (letter.empty()) {
        st.error = T_("This partition has no drive letter.");
        return;
    }
    size_t blobSize = 0;
    const uint8_t* blob = grub4dosGrldrBlob(&blobSize);
    if (!blob || blobSize == 0) {
        st.error = T_("Boot code not bundled in this build.");
        return;
    }
    const std::string path = letter + "/GRLDR"; // '/' works on Win32 and POSIX
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        st.error = std::string(T_("Cannot open file: ")) + path;
        return;
    }
    f.write(reinterpret_cast<const char*>(blob), std::streamsize(blobSize));
    f.close();
    if (!f) {
        st.error = std::string(T_("Cannot write backup file: ")) + path;
        return;
    }
    st.info = std::string(T_("Saved: ")) + path;
    st.error.clear();
}

} // namespace

void Grub4dosScreen::drawBody(App& app)
{
    GrubUi& st = ui();
    const DiskInfo* disk = app.currentDisk();
    if (disk && st.partIndex >= (int)disk->partitions.size()) {
        st.partIndex = 0;
        st.loaded = false;
    }

    // --- target row -----------------------------------------------------------------
    ImGui::TextUnformatted(T_("Target:"));
    ImGui::SameLine();
    if (DiskPicker::draw(app, 360, "grub")) {
        st.partIndex = 0;
        st.loaded = false;
    }

    if (!disk || disk->partitions.empty()) {
        ImGui::TextDisabled("%s", T_("This disk has no partitions."));
    } else {
        const std::string cur = partitionLabel(disk->partitions[st.partIndex]);
        ImGui::SetNextItemWidth(360);
        if (ImGui::BeginCombo("##grubpart", cur.c_str())) {
            for (int i = 0; i < (int)disk->partitions.size(); ++i) {
                const std::string label = partitionLabel(disk->partitions[i]);
                if (ImGui::Selectable(label.c_str(), i == st.partIndex)) {
                    st.partIndex = i;
                    st.loaded = false;
                }
                if (i == st.partIndex) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Read"))) {
        std::string err;
        if (readTargets(app, st, &err)) {
            st.error.clear();
            st.info.clear();
        } else {
            st.loaded = false;
            st.error = err;
        }
    }

    // Auto-inspect once per distinct target.
    const std::string wantKey =
        std::to_string(app.selectedDisk()) + "/" + std::to_string(st.partIndex);
    if (!st.loaded && st.triedKey != wantKey && disk != nullptr) {
        st.triedKey = wantKey;
        std::string err;
        if (readTargets(app, st, &err)) {
            st.error.clear();
        } else {
            st.loaded = false;
            st.error = err;
        }
    }

    // --- current boot code ---------------------------------------------------------------
    ImGui::Spacing();
    if (st.loaded) {
        ImGui::TextUnformatted(T_("Current MBR:"));
        ImGui::SameLine();
        std::string kind = T_(std::string(mbrKindLabel(st.mbrKind)).c_str());
        ImGui::TextColored(kColValue, "%s", kind.c_str());
        if (!st.pbrSector.empty()) {
            ImGui::SameLine();
            ImGui::TextUnformatted(T_("Current PBR:"));
            ImGui::SameLine();
            kind = T_(std::string(pbrKindLabel(st.pbrKind)).c_str());
            ImGui::TextColored(kColValue, "%s", kind.c_str());
        }
    } else {
        ImGui::TextDisabled("%s", T_("Select a target to inspect its boot sector."));
    }

    // --- install boot code -----------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(T_("Install Grub4DOS boot code:"));
    if (ImGui::RadioButton(T_("Master boot record (grldr.mbr)"), &st.codeTarget, 0)) {
        st.error.clear();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(T_("Partition boot record (grldr.pbr)"), &st.codeTarget, 1)) {
        st.error.clear();
    }

    const bool pbrReady = st.codeTarget == 0 || !disk->partitions.empty();
    ImGui::BeginDisabled(!st.loaded || !pbrReady);
    if (ImGui::Button(T_("Install"))) {
        installBootCode(app, st);
    }
    ImGui::EndDisabled();

    // --- GRLDR file ----------------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(T_("GRLDR file:"));
    ImGui::SameLine();
    if (ImGui::Button(T_("Write GRLDR to volume root"))) {
        writeGrldrFile(app, st);
    }

    // --- menu.lst editor -----------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(T_("menu.lst:"));
    const bool haveLetter = disk && !disk->partitions.empty() &&
                            !disk->partitions[st.partIndex].driveLetter.empty();
    ImGui::BeginDisabled(!haveLetter);
    if (ImGui::Button(T_("Read from volume"))) {
        const std::string letter = disk->partitions[st.partIndex].driveLetter;
        const std::string path = letter + "\\menu.lst";
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            st.error = std::string(T_("Cannot open file: ")) + path;
        } else {
            std::string text((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
            f.close();
            text.resize(std::min(text.size(), sizeof(st.menuText) - 1));
            std::snprintf(st.menuText, sizeof(st.menuText), "%s", text.c_str());
            st.info = std::string(T_("Saved: ")) + path;
            st.error.clear();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Write to volume"))) {
        const std::string letter = disk->partitions[st.partIndex].driveLetter;
        const std::string path = letter + "\\menu.lst";
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            st.error = std::string(T_("Cannot open file: ")) + path;
        } else {
            f.write(st.menuText, std::streamsize(std::strlen(st.menuText)));
            f.close();
            if (f) {
                st.info = std::string(T_("Saved: ")) + path;
                st.error.clear();
            } else {
                st.error = std::string(T_("Cannot write backup file: ")) + path;
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T_("Load from file..."))) {
        const std::string p = app.platform()->openFileDialog(
            T_("Open menu.lst"), std::string(T_("All files")) + "|*.*");
        if (!p.empty()) {
            std::ifstream f(p, std::ios::binary);
            if (!f) {
                st.error = std::string(T_("Cannot open file: ")) + p;
            } else {
                std::string text((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
                text.resize(std::min(text.size(), sizeof(st.menuText) - 1));
                std::snprintf(st.menuText, sizeof(st.menuText), "%s", text.c_str());
                st.info = std::string(T_("Saved: ")) + p;
                st.error.clear();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Save to file..."))) {
        const std::string p = app.platform()->saveFileDialog(
            T_("Save menu.lst as"), std::string(T_("All files")) + "|*.*", "lst");
        if (!p.empty()) {
            std::ofstream f(p, std::ios::binary);
            if (!f) {
                st.error = std::string(T_("Cannot open file: ")) + p;
            } else {
                f.write(st.menuText, std::streamsize(std::strlen(st.menuText)));
                f.close();
                if (f) {
                    st.info = std::string(T_("Saved: ")) + p;
                    st.error.clear();
                } else {
                    st.error = std::string(T_("Cannot write backup file: ")) + p;
                }
            }
        }
    }
    ImGui::InputTextMultiline("##grubmenu", st.menuText, sizeof(st.menuText),
                              ImVec2(-1.0f, ImGui::GetTextLineHeight() * 12.0f));

    // --- status ------------------------------------------------------------------------------------
    if (!st.error.empty()) {
        ImGui::TextColored(kColError, "%s", st.error.c_str());
    } else if (!st.info.empty()) {
        ImGui::TextColored(kColOk, "%s", st.info.c_str());
    }
}

} // namespace bootroll
