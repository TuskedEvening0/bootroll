#include "ui/MbrScreen.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/bootcode/BootCode.h"
#include "ui/widgets/DiskPicker.h"
#include "imgui.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
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

// Writes the automatic safety copy of the bytes that are about to be replaced.
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
    std::time_t t = std::time(nullptr);
    std::tm tmv {};
    localtime_s(&tmv, &t);
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

std::string sanitizeTag(std::string s)
{
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '"') {
            c = '_';
        }
    }
    if (s.size() > 48) {
        s = s.substr(s.size() - 48); // keep the tail (file name)
    }
    return s;
}

struct InstallItem {
    MbrInstallType type;
    const char* label; // translatable source string
};

const InstallItem kInstalls[] = {
    { MbrInstallType::Nt6,      "Windows NT 6.x MBR (bootroll)" },
    { MbrInstallType::Grub4dos, "Grub4DOS MBR (grldr)" },
    { MbrInstallType::Wee,      "WEE boot manager" },
    { MbrInstallType::Syslinux, "Syslinux MBR" },
};

struct MbrUi {
    int target = 0;               // 0 = physical disk, 1 = raw image file
    char filePath[1024] = {};
    bool loaded = false;          // cached sector 0 valid
    std::string triedKey;         // target signature of the last auto-read attempt
    std::vector<uint8_t> sector;  // cached target sector 0
    MbrKind kind = MbrKind::Unknown;
    int installType = 0;          // index into kInstalls
    std::string error;            // red status line
    std::string info;             // green status line
};

MbrUi& ui()
{
    static MbrUi s;
    return s;
}

bool readTarget(App& app, MbrUi& st, std::string* err)
{
    st.sector.assign(kSector, 0);
    if (st.target == 0) {
        const DiskInfo* d = app.currentDisk();
        if (!d) {
            *err = T_("No disk selected.");
            return false;
        }
        try {
            app.diskAccess()->readSectors(d->number, 0, st.sector.data(), st.sector.size());
        } catch (const std::exception& e) {
            *err = e.what();
            return false;
        }
    } else {
        if (st.filePath[0] == '\0') {
            *err = T_("No file specified.");
            return false;
        }
        std::ifstream f(st.filePath, std::ios::binary);
        if (!f) {
            *err = std::string(T_("Cannot open file: ")) + st.filePath;
            return false;
        }
        f.read(reinterpret_cast<char*>(st.sector.data()), std::streamsize(kSector));
        if (f.gcount() < std::streamsize(kSector)) {
            *err = T_("File is smaller than one sector.");
            return false;
        }
    }
    st.kind = detectMbrKind(st.sector.data(), st.sector.size());
    st.loaded = true;
    return true;
}

bool writeStaged(App& app, MbrUi& st, const std::vector<uint8_t>& bytes, std::string* err)
{
    if (st.target == 0) {
        const DiskInfo* d = app.currentDisk();
        if (!d) {
            *err = T_("No disk selected.");
            return false;
        }
        try {
            app.diskAccess()->writeSectors(d->number, 0, bytes.data(), bytes.size());
            app.diskAccess()->flush(d->number);
        } catch (const std::exception& e) {
            *err = e.what();
            return false;
        }
    } else {
        // Rewrite the leading bytes in place (file must hold them all).
        namespace fs = std::filesystem;
        std::error_code ec;
        const uintmax_t fsize = fs::file_size(st.filePath, ec);
        if (ec || fsize < bytes.size()) {
            *err = T_("File is smaller than one sector.");
            return false;
        }
        std::fstream f(st.filePath, std::ios::in | std::ios::out | std::ios::binary);
        if (!f) {
            *err = std::string(T_("Cannot open file: ")) + st.filePath;
            return false;
        }
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        f.close();
        if (!f) {
            *err = std::string(T_("Write failed: ")) + st.filePath;
            return false;
        }
    }
    return true;
}

// Reads exactly n leading bytes of the target (used to back up everything a
// staged install is about to overwrite, including the extra MBR sectors).
bool readTargetBytes(App& app, MbrUi& st, size_t bytes, std::vector<uint8_t>* out,
                     std::string* err)
{
    out->assign(bytes, 0);
    if (st.target == 0) {
        const DiskInfo* d = app.currentDisk();
        if (!d) {
            *err = T_("No disk selected.");
            return false;
        }
        try {
            app.diskAccess()->readSectors(d->number, 0, out->data(), out->size());
        } catch (const std::exception& e) {
            *err = e.what();
            return false;
        }
        return true;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const uintmax_t fsize = fs::file_size(st.filePath, ec);
    if (ec || fsize < bytes) {
        *err = T_("File is smaller than one sector.");
        return false;
    }
    std::ifstream f(st.filePath, std::ios::binary);
    if (!f) {
        *err = std::string(T_("Cannot open file: ")) + st.filePath;
        return false;
    }
    f.read(reinterpret_cast<char*>(out->data()), std::streamsize(bytes));
    if ((size_t)f.gcount() < bytes) {
        *err = T_("File is smaller than one sector.");
        return false;
    }
    return true;
}

// Fresh target read + automatic backup + native confirm; writes on OK.
// kind: 1 = install, 2 = restore.
void confirmAndWrite(App& app, MbrUi& st, std::vector<uint8_t> staged, int kind)
{
    std::string err;
    std::vector<uint8_t> backup; // everything the staged write overwrites
    if (!readTargetBytes(app, st, staged.size(), &backup, &err)) {
        st.error = err;
        return;
    }
    std::string tag = st.target == 0
        ? "mbr_disk" + std::to_string(app.currentDisk()->number)
        : "mbr_file_" + sanitizeTag(st.filePath);
    std::string bpath;
    if (!writeAutoBackup(app, tag, backup.data(), backup.size(), &bpath, &err)) {
        st.error = err;
        return;
    }
    // Question line depends on the operation and the code size.
    char head[192];
    if (kind == 1) {
        if (staged.size() > 512) {
            std::snprintf(
                head, sizeof(head),
                T_("Install this MBR? The first %u sectors of the target will be overwritten."),
                (unsigned)(staged.size() / 512));
        } else {
            std::snprintf(head, sizeof(head), "%s",
                          T_("Install this MBR? The first 440 bytes of the target will be overwritten."));
        }
    } else {
        std::snprintf(head, sizeof(head), "%s",
                      T_("Restore this backup? The first sector of the target will be overwritten."));
    }
    const std::string text = std::string(head) + "\n\n" + T_("Automatic backup saved to") +
                             ": " + bpath;
    if (app.platform()->confirmDialog(T_("Confirm write"), text)) {
        if (writeStaged(app, st, staged, &err)) {
            st.info = kind == 1 ? T_("MBR installed.") : T_("Backup restored.");
            st.error.clear();
            st.loaded = false;   // refresh the readout from disk next time
            st.triedKey.clear();
        } else {
            st.error = err;
        }
    }
}

} // namespace

void MbrScreen::drawBody(App& app)
{
    MbrUi& st = ui();

    // --- target row ---------------------------------------------------------------
    ImGui::TextUnformatted(T_("Target:"));
    ImGui::SameLine();
    if (ImGui::RadioButton(T_("Disk"), &st.target, 0)) {
        st.loaded = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(T_("Image file"), &st.target, 1)) {
        st.loaded = false;
    }

    if (st.target == 0) {
        if (DiskPicker::draw(app, 420, "mbr")) {
            st.loaded = false;
        }
    } else {
        ImGui::SetNextItemWidth(420);
        if (ImGui::InputText("##mbrfile", st.filePath, sizeof(st.filePath))) {
            st.loaded = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("...##mbrfiledlg")) {
            const std::string p = app.platform()->openFileDialog(
                T_("Open image file"), std::string(T_("All files")) + "|*.*");
            if (!p.empty()) {
                std::snprintf(st.filePath, sizeof(st.filePath), "%s", p.c_str());
                st.loaded = false;
            }
        }
    }

    // Auto-inspect once per distinct target (a single 512-byte read).
    const std::string wantKey = st.target == 0
        ? "d" + std::to_string(app.selectedDisk())
        : "f" + std::string(st.filePath);
    const bool targetReachable = st.target == 0 ? app.currentDisk() != nullptr
                                                : st.filePath[0] != '\0';
    if (!st.loaded && st.triedKey != wantKey && targetReachable) {
        st.triedKey = wantKey;
        std::string err;
        if (readTarget(app, st, &err)) {
            st.error.clear();
        } else {
            st.loaded = false;
            st.error = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Read sector 0"))) {
        std::string err;
        if (readTarget(app, st, &err)) {
            st.error.clear();
            st.info.clear();
        } else {
            st.loaded = false;
            st.error = err;
        }
    }

    // --- current MBR ----------------------------------------------------------------
    ImGui::Spacing();
    if (st.loaded) {
        ImGui::TextUnformatted(T_("Current MBR:"));
        ImGui::SameLine();
        const std::string kind = T_(std::string(mbrKindLabel(st.kind)).c_str());
        ImGui::TextColored(kColValue, "%s", kind.c_str());
        if (st.target == 0) {
            const DiskInfo* d = app.currentDisk();
            if (d && d->style == PartitionStyle::Gpt) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", T_("GPT protective MBR"));
            }
        }
    } else {
        ImGui::TextDisabled("%s", T_("Select a target to inspect its boot sector."));
    }

    // --- install ----------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(T_("Install MBR:"));
    ImGui::SetNextItemWidth(340);
    if (ImGui::BeginCombo("##mbrinstall", T_(kInstalls[st.installType].label))) {
        for (int i = 0; i < (int)std::size(kInstalls); ++i) {
            if (ImGui::Selectable(T_(kInstalls[i].label), i == st.installType)) {
                st.installType = i;
            }
            if (i == st.installType) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    MbrInstallInfo info {};
    const bool haveCode = mbrInstallInfo(kInstalls[st.installType].type, &info);

    ImGui::BeginDisabled(!st.loaded || !haveCode);
    if (ImGui::Button(T_("Install"))) {
        std::vector<uint8_t> staged;
        if (!stageMbrInstall(kInstalls[st.installType].type, st.sector.data(),
                             st.sector.size(), &staged)) {
            st.error = T_("Cannot apply the MBR code to this target.");
        } else {
            confirmAndWrite(app, st, std::move(staged), 1);
        }
    }
    ImGui::EndDisabled();
    if (!haveCode) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
                            T_("Boot code not bundled in this build."));
    } else if (info.multiSector) {
        const MbrInstallType sel = kInstalls[st.installType].type;
        const unsigned nSectors =
            sel == MbrInstallType::Wee ? 63u : (unsigned)(info.blobSize / 512);
        char hint[96];
        std::snprintf(hint, sizeof(hint), T_("This boot code occupies the first %u sectors."),
                      nSectors);
        ImGui::TextDisabled("%s", hint);
    } else if (kInstalls[st.installType].type == MbrInstallType::Nt6) {
        ImGui::TextWrapped(
            "%s", T_("For Windows Vista and later. Locates the active partition and "
                     "hands control to it to load bootmgr. Works on both MBR and GPT disks."));
    }

    // --- backup / restore ---------------------------------------------------------------
    ImGui::SameLine();
    if (ImGui::Button(T_("Backup..."))) {
        std::string err;
        if (!readTarget(app, st, &err)) {
            st.error = err;
        } else {
            const std::string p = app.platform()->saveFileDialog(
                T_("Backup boot sector as"),
                std::string(T_("Boot sector files")) + "|*.bin|" + T_("All files") + "|*.*",
                "bin");
            if (!p.empty()) {
                std::ofstream f(p, std::ios::binary);
                f.write(reinterpret_cast<const char*>(st.sector.data()),
                        std::streamsize(st.sector.size()));
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
    ImGui::SameLine();
    if (ImGui::Button(T_("Restore..."))) {
        const std::string p = app.platform()->openFileDialog(
            T_("Open backup file"),
            std::string(T_("Boot sector files")) + "|*.bin|" + T_("All files") + "|*.*");
        if (!p.empty()) {
            std::vector<uint8_t> buf(kSector);
            std::ifstream f(p, std::ios::binary);
            f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(kSector));
            if (f.gcount() < std::streamsize(kSector)) {
                st.error = T_("File is smaller than one sector.");
            } else {
                confirmAndWrite(app, st, std::move(buf), 2);
            }
        }
    }

    // --- status ---------------------------------------------------------------------------
    if (!st.error.empty()) {
        ImGui::TextColored(kColError, "%s", st.error.c_str());
    } else if (!st.info.empty()) {
        ImGui::TextColored(kColOk, "%s", st.info.c_str());
    }
}

} // namespace bootroll
