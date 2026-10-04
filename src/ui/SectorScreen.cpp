#include "ui/SectorScreen.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/disk/IDiskAccess.h"
#include "ui/widgets/DiskPicker.h"
#include "ui/widgets/HexView.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bootroll {

namespace {

constexpr size_t kFileSector = 512;

static const ImVec4 kColError = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
static const ImVec4 kColOk = ImVec4(0.5f, 0.8f, 0.5f, 1.0f);
static const ImVec4 kColWarn = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);

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

std::string sanitizeTag(const std::string& path)
{
    std::string s;
    for (const char c : path) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z');
        s += ok ? c : '_';
    }
    if (s.size() > 40) {
        s = s.substr(s.size() - 40);
    }
    return s;
}

struct SectorUi {
    int target = 0;               // 0 = physical disk, 1 = raw image file
    char filePath[1024] = {};
    bool loaded = false;          // current sector buffer valid
    std::string triedKey;         // target signature of the last auto-read
    std::vector<uint8_t> data;    // current sector (sectorSize bytes)
    uint64_t lba = 0;             // current sector number
    uint64_t totalSectors = 0;    // of the current target
    uint32_t sectorSize = kFileSector;
    hexedit::State hex;           // hex grid cursor state
    bool dirty = false;           // buffer differs from the target
    std::string error;
    std::string info;
};

SectorUi& ui()
{
    static SectorUi s;
    return s;
}

uint64_t fileSectorCount(const char* path)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : size / kFileSector;
}

bool readTargetSector(App& app, SectorUi& st, std::vector<uint8_t>* out,
                      std::string* err)
{
    if (st.target == 0) {
        const DiskInfo* d = app.currentDisk();
        if (!d) {
            *err = T_("No disk selected.");
            return false;
        }
        out->assign(st.data.size(), 0);
        try {
            app.diskAccess()->readSectors(d->number, st.lba * st.sectorSize,
                                          out->data(), out->size());
        } catch (const std::exception& e) {
            *err = e.what();
            return false;
        }
        return true;
    }
    std::ifstream f(st.filePath, std::ios::binary);
    if (!f) {
        *err = std::string(T_("Cannot open file: ")) + st.filePath;
        return false;
    }
    out->assign(st.data.size(), 0);
    f.seekg(std::streamoff(st.lba * st.sectorSize));
    f.read(reinterpret_cast<char*>(out->data()), std::streamsize(out->size()));
    if (f.gcount() < std::streamsize(out->size())) {
        *err = T_("File is smaller than one sector.");
        return false;
    }
    return true;
}

bool readCurrent(App& app, SectorUi& st, std::string* err)
{
    if (st.target == 0) {
        const DiskInfo* d = app.currentDisk();
        if (!d) {
            *err = T_("No disk selected.");
            return false;
        }
        st.sectorSize = d->sectorSize ? d->sectorSize : 512;
        st.totalSectors = d->totalBytes / st.sectorSize;
    } else {
        if (st.filePath[0] == '\0') {
            *err = T_("No file specified.");
            return false;
        }
        st.sectorSize = kFileSector;
        st.totalSectors = fileSectorCount(st.filePath);
    }
    if (st.totalSectors == 0) {
        *err = T_("LBA out of range.");
        return false;
    }
    if (st.lba >= st.totalSectors) {
        st.lba = st.totalSectors - 1;
    }
    st.data.assign(st.sectorSize, 0);
    if (!readTargetSector(app, st, &st.data, err)) {
        return false;
    }
    st.loaded = true;
    st.dirty = false;
    st.hex = hexedit::State {};
    return true;
}

// Navigation is refused while edits are unwritten so nothing is lost silently.
void navigateTo(App& app, SectorUi& st, uint64_t newLba)
{
    if (st.dirty) {
        st.error = T_("Unwritten changes. Write or revert first.");
        return;
    }
    st.lba = newLba;
    std::string err;
    if (readCurrent(app, st, &err)) {
        st.error.clear();
        st.info.clear();
    } else {
        st.loaded = false;
        st.error = err;
    }
}

// Fresh target read + automatic backup + native confirm; writes on OK.
bool writeStaged(App& app, SectorUi& st, const std::vector<uint8_t>& bytes, std::string* err);
void confirmAndWrite(App& app, SectorUi& st)
{
    std::vector<uint8_t> backup;
    std::string err;
    if (!readTargetSector(app, st, &backup, &err)) {
        st.error = err;
        return;
    }
    const std::string tag = st.target == 0
        ? "sector_disk" + std::to_string(app.currentDisk()->number) +
              "_lba" + std::to_string(st.lba)
        : "sector_file_" + sanitizeTag(st.filePath);
    std::string bpath;
    if (!writeAutoBackup(app, tag, backup.data(), backup.size(), &bpath, &err)) {
        st.error = err;
        return;
    }
    char head[160];
    std::snprintf(head, sizeof(head),
                  T_("Write this sector? LBA %llu of the target will be overwritten."),
                  (unsigned long long)st.lba);
    const std::string text = std::string(head) + "\n\n" + T_("Automatic backup saved to") +
                             ": " + bpath;
    if (app.platform()->confirmDialog(T_("Confirm write"), text)) {
        if (writeStaged(app, st, st.data, &err)) {
            st.info = T_("Sector written.");
            st.error.clear();
            st.dirty = false;
            st.loaded = false;
            st.triedKey.clear(); // refresh the readout from the target
        } else {
            st.error = err;
        }
    }
}

bool writeStaged(App& app, SectorUi& st, const std::vector<uint8_t>& bytes, std::string* err)
{
    if (st.target == 0) {
        const DiskInfo* d = app.currentDisk();
        if (!d) {
            *err = T_("No disk selected.");
            return false;
        }
        try {
            app.diskAccess()->writeSectors(d->number, st.lba * st.sectorSize,
                                           bytes.data(), bytes.size());
            app.diskAccess()->flush(d->number);
        } catch (const std::exception& e) {
            *err = e.what();
            return false;
        }
        return true;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const uintmax_t fsize = fs::file_size(st.filePath, ec);
    const uint64_t need = st.lba * st.sectorSize + bytes.size();
    if (ec || fsize < need) {
        *err = T_("File is smaller than one sector.");
        return false;
    }
    std::fstream f(st.filePath, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) {
        *err = std::string(T_("Cannot open file: ")) + st.filePath;
        return false;
    }
    f.seekp(std::streamoff(st.lba * st.sectorSize));
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    f.close();
    if (!f) {
        *err = std::string(T_("Cannot save file: ")) + st.filePath;
        return false;
    }
    return true;
}

} // namespace

void SectorScreen::drawBody(App& app)
{
    SectorUi& st = ui();

    // --- target row ---------------------------------------------------------------
    ImGui::TextUnformatted(T_("Target:"));
    ImGui::SameLine();
    if (ImGui::RadioButton(T_("Disk"), &st.target, 0)) {
        st.loaded = false;
        st.lba = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(T_("Image file"), &st.target, 1)) {
        st.loaded = false;
        st.lba = 0;
    }

    if (st.target == 0) {
        if (DiskPicker::draw(app, 420, "sector")) {
            st.loaded = false;
            st.lba = 0;
        }
    } else {
        ImGui::SetNextItemWidth(420);
        if (ImGui::InputText("##sectorfile", st.filePath, sizeof(st.filePath))) {
            st.loaded = false;
            st.lba = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("...##sectorfiledlg")) {
            const std::string p = app.platform()->openFileDialog(
                T_("Open image file"), std::string(T_("All files")) + "|*.*");
            if (!p.empty()) {
                std::snprintf(st.filePath, sizeof(st.filePath), "%s", p.c_str());
                st.loaded = false;
                st.lba = 0;
            }
        }
    }

    // Auto-inspect once per distinct target (a single sector read).
    const std::string wantKey = st.target == 0
        ? "d" + std::to_string(app.selectedDisk())
        : "f" + std::string(st.filePath);
    const bool targetReachable = st.target == 0 ? app.currentDisk() != nullptr
                                                : st.filePath[0] != '\0';
    if (!st.loaded && st.triedKey != wantKey && targetReachable) {
        st.triedKey = wantKey;
        std::string err;
        if (readCurrent(app, st, &err)) {
            st.error.clear();
        } else {
            st.loaded = false;
            st.error = err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Read"))) {
        std::string err;
        if (readCurrent(app, st, &err)) {
            st.error.clear();
            st.info.clear();
        } else {
            st.loaded = false;
            st.error = err;
        }
    }

    // --- sector navigation ----------------------------------------------------------
    ImGui::Spacing();
    ImGui::TextUnformatted(T_("Sector LBA:"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    ImGui::InputScalar("##sectorlba", ImGuiDataType_U64, &st.lba, nullptr,
                       nullptr, "%llu");
    ImGui::SameLine();
    if (ImGui::Button(T_("Previous sector")) && st.lba > 0) {
        navigateTo(app, st, st.lba - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Next sector"))) {
        navigateTo(app, st, st.lba + 1);
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Goto"))) {
        navigateTo(app, st, st.lba);
    }
    ImGui::SameLine();
    if (st.loaded) {
        char buf[80];
        std::snprintf(buf, sizeof(buf), T_("Total sectors: %llu"),
                      static_cast<unsigned long long>(st.totalSectors));
        ImGui::TextDisabled("%s", buf);
    }

    // --- hex view ---------------------------------------------------------------------
    ImGui::Spacing();
    if (!st.loaded) {
        ImGui::TextDisabled("%s", T_("Select a target to inspect its boot sector."));
    } else {
        if (st.dirty) {
            ImGui::TextColored(kColWarn, "* %s", T_("Unwritten changes."));
        }
        if (HexView::draw(st.data, st.hex, app.hexFont())) {
            st.dirty = true;
            st.info.clear();
        }

        // --- actions ----------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::BeginDisabled(!st.dirty);
        if (ImGui::Button(T_("Write to disk"))) {
            confirmAndWrite(app, st);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!st.dirty);
        if (ImGui::Button(T_("Revert"))) {
            std::string err;
            if (readCurrent(app, st, &err)) {
                st.error.clear();
                st.info.clear();
            } else {
                st.loaded = false;
                st.error = err;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T_("Save sector to file..."))) {
            const std::string p = app.platform()->saveFileDialog(
                T_("Save sector as"),
                std::string(T_("Boot sector files")) + "|*.bin|" + T_("All files") + "|*.*",
                "bin");
            if (!p.empty()) {
                std::ofstream f(p, std::ios::binary);
                if (!f) {
                    st.error = std::string(T_("Cannot save file: ")) + p;
                } else {
                    f.write(reinterpret_cast<const char*>(st.data.data()),
                            std::streamsize(st.data.size()));
                    f.close();
                    if (!f) {
                        st.error = std::string(T_("Cannot save file: ")) + p;
                    } else {
                        st.info = T_("Sector saved.");
                        st.error.clear();
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T_("Restore sector from file..."))) {
            const std::string p = app.platform()->openFileDialog(
                T_("Open sector file"), std::string(T_("All files")) + "|*.*");
            if (!p.empty()) {
                std::ifstream f(p, std::ios::binary);
                if (!f) {
                    st.error = std::string(T_("Cannot open file: ")) + p;
                } else {
                    std::vector<uint8_t> buf(st.data.size(), 0);
                    f.read(reinterpret_cast<char*>(buf.data()),
                           std::streamsize(buf.size()));
                    const std::streamsize got = f.gcount();
                    if (got <= 0) {
                        st.error = T_("File is smaller than one sector.");
                    } else {
                        std::fill(buf.begin() + got, buf.end(), uint8_t(0));
                        st.data = buf;
                        st.dirty = true;
                        st.info = T_("Loaded from file. Unwritten changes.");
                        st.error.clear();
                    }
                }
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
