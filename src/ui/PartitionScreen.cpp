#include "ui/PartitionScreen.h"
#include "ui/ElevateHint.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/util/LocalTime.h"
#include "core/disk/DiskInfo.h"
#include "core/disk/PartitionTable.h"
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

static const ImVec4 kColInfo = ImVec4(0.5f, 0.55f, 0.6f, 1.0f);
static const ImVec4 kColError = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
static const ImVec4 kColOk = ImVec4(0.5f, 0.8f, 0.5f, 1.0f);
static const ImVec4 kColWarn = ImVec4(0.95f, 0.8f, 0.3f, 1.0f);

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

std::string sanitizeTag(std::string s)
{
    for (char& c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == ' ' || c == '"') {
            c = '_';
        }
    }
    if (s.size() > 48) {
        s = s.substr(s.size() - 48);
    }
    return s;
}

// Common type bytes offered by the "Set type" combo (BOOTICE-compatible set).
struct TypeItem {
    uint8_t type;
    const char* label;
};

const TypeItem kTypes[] = {
    { 0x07, "NTFS / exFAT" },
    { 0x17, "NTFS / exFAT (hidden)" },
    { 0x0B, "FAT32" },
    { 0x1B, "FAT32 (hidden)" },
    { 0x0C, "FAT32 (LBA)" },
    { 0x1C, "FAT32 (LBA, hidden)" },
    { 0x06, "FAT16" },
    { 0x16, "FAT16 (hidden)" },
    { 0x0E, "FAT16 (LBA)" },
    { 0x1E, "FAT16 (LBA, hidden)" },
    { 0x01, "FAT12" },
    { 0x11, "FAT12 (hidden)" },
    { 0x05, "Extended" },
    { 0x0F, "Extended (LBA)" },
    { 0x12, "EISA / Diagnostic" },
    { 0x27, "NT Hidden / WinRE" },
    { 0x83, "Linux" },
    { 0x82, "Linux swap" },
    { 0xEF, "EFI System" },
};

struct PartUi {
    int target = 0;               // 0 = physical disk, 1 = raw image file
    char filePath[1024] = {};
    bool loaded = false;          // cached sector 0 valid
    std::string triedKey;         // target signature of the last auto-read attempt
    std::vector<uint8_t> sector;  // cached (and possibly edited) sector 0
    bool dirty = false;           // in-memory edits not written yet
    bool gpt = false;             // GPT protective MBR -> read-only
    int selected = -1;            // selected entry (0..3), -1 = none
    int typeIndex = 0;            // index into kTypes for "Set type"
    std::string error;
    std::string info;
};

PartUi& ui()
{
    static PartUi s;
    return s;
}

bool readTarget(App& app, PartUi& st, std::string* err)
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
    const MbrTable t = parseMbr(st.sector.data(), st.sector.size());
    st.gpt = !t.entries.empty() && t.entries[0].type == 0xEE;
    st.dirty = false;
    st.selected = -1;
    st.loaded = true;
    return true;
}

bool writeStaged(App& app, PartUi& st, const std::vector<uint8_t>& bytes, std::string* err)
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

// Fresh target read + automatic backup + native confirm; writes on OK.
void confirmAndWrite(App& app, PartUi& st, const std::vector<uint8_t>& bytes)
{
    std::string err;
    std::vector<uint8_t> backup(bytes.size(), 0);
    if (st.target == 0) {
        const DiskInfo* d = app.currentDisk();
        if (!d) {
            st.error = T_("No disk selected.");
            return;
        }
        try {
            app.diskAccess()->readSectors(d->number, 0, backup.data(), backup.size());
        } catch (const std::exception& e) {
            st.error = e.what();
            return;
        }
    } else {
        std::ifstream f(st.filePath, std::ios::binary);
        if (!f) {
            st.error = std::string(T_("Cannot open file: ")) + st.filePath;
            return;
        }
        f.read(reinterpret_cast<char*>(backup.data()), std::streamsize(backup.size()));
        if ((size_t)f.gcount() < backup.size()) {
            st.error = T_("File is smaller than one sector.");
            return;
        }
    }
    const std::string tag = st.target == 0
        ? "part_disk" + std::to_string(app.currentDisk()->number)
        : "part_file_" + sanitizeTag(st.filePath);
    std::string bpath;
    if (!writeAutoBackup(app, tag, backup.data(), backup.size(), &bpath, &err)) {
        st.error = err;
        return;
    }
    const std::string text = std::string(T_("Write the partition table? Sector 0 of the target will be overwritten.")) +
                             "\n\n" + T_("Automatic backup saved to") + ": " + bpath;
    if (app.platform()->confirmDialog(T_("Confirm write"), text)) {
        if (writeStaged(app, st, bytes, &err)) {
            st.info = T_("Partition table written.");
            st.error.clear();
            st.dirty = false;
            st.loaded = false;   // refresh the readout from disk next time
            st.triedKey.clear();
        } else {
            st.error = err;
        }
    }
}

void drawTable(PartUi& st)
{
    const MbrTable t = parseMbr(st.sector.data(), st.sector.size());

    static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##parttable", 6, flags)) {
        return;
    }
    ImGui::TableSetupColumn(T_("No."), ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn(T_("Active"), ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn(T_("Type"), ImGuiTableColumnFlags_WidthFixed, 200);
    ImGui::TableSetupColumn(T_("Boot LBA"), ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn(T_("Size"), ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn(T_("End LBA"));
    ImGui::TableHeadersRow();

    for (int i = 0; i < (int)t.entries.size(); ++i) {
        const MbrEntry& e = t.entries[i];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);

        char num[16], lbaBegin[24], lbaEnd[24], size[32];
        std::snprintf(num, sizeof(num), "%d%s", i + 1, (i == st.selected && st.dirty) ? " *" : "");

        const bool selectable = !e.empty && ImGui::Selectable(num, i == st.selected);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(e.active ? "*" : "");
        ImGui::TableSetColumnIndex(2);
        if (e.empty) {
            ImGui::TextDisabled("%s", T_("Empty"));
        } else {
            char type[96];
            const std::string label(mbrPartitionTypeLabel(e.type));
            if (!label.empty()) {
                std::snprintf(type, sizeof(type), "0x%02X  %s", e.type, T_(label.c_str()));
            } else {
                std::snprintf(type, sizeof(type), "0x%02X", e.type);
            }
            ImGui::TextUnformatted(type);
        }
        ImGui::TableSetColumnIndex(3);
        std::snprintf(lbaBegin, sizeof(lbaBegin), "%u", e.beginLba);
        ImGui::TextUnformatted(e.empty ? "" : lbaBegin);
        ImGui::TableSetColumnIndex(4);
        std::snprintf(size, sizeof(size), "%s",
                      e.empty ? "" : formatSize(uint64_t(e.sectorCount) * kSector).c_str());
        ImGui::TextUnformatted(size);
        ImGui::TableSetColumnIndex(5);
        std::snprintf(lbaEnd, sizeof(lbaEnd), "%u",
                      e.empty ? 0u : e.beginLba + (e.sectorCount ? e.sectorCount - 1 : 0));
        ImGui::TextUnformatted(e.empty ? "" : lbaEnd);

        if (selectable) {
            st.selected = i;
        }
    }
    ImGui::EndTable();
}

} // namespace

void PartitionScreen::drawBody(App& app)
{
    PartUi& st = ui();

    elevate::maybeShowModal(app);
    elevate::drawBanner(app);

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
        if (DiskPicker::draw(app, 420, "part")) {
            st.loaded = false;
        }
    } else {
        ImGui::SetNextItemWidth(420);
        if (ImGui::InputText("##partfile", st.filePath, sizeof(st.filePath))) {
            st.loaded = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("...##partfiledlg")) {
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
    if (ImGui::Button(T_("Read"))) {
        std::string err;
        if (readTarget(app, st, &err)) {
            st.error.clear();
            st.info.clear();
        } else {
            st.loaded = false;
            st.error = err;
        }
    }

    // --- table -----------------------------------------------------------------------
    ImGui::Spacing();
    if (!st.loaded) {
        ImGui::TextDisabled("%s", T_("Select a target to inspect its boot sector."));
    } else {
        if (st.dirty) {
            ImGui::TextColored(kColWarn, "* %s", T_("Unwritten changes."));
            ImGui::SameLine();
        }
        if (st.gpt) {
            ImGui::TextColored(kColInfo, "%s",
                               T_("GPT disk: the protective MBR is shown read-only."));
        }
        drawTable(st);

        const MbrTable t = parseMbr(st.sector.data(), st.sector.size());
        const bool haveSel = st.selected >= 0 && st.selected < (int)t.entries.size();
        const bool entryEditable = haveSel && !st.gpt;
        const MbrEntry& sel = entryEditable ? t.entries[st.selected] : MbrEntry {};

        // --- row operations ------------------------------------------------------------
        ImGui::Spacing();
        ImGui::BeginDisabled(!entryEditable);
        if (ImGui::Button(T_("Set active"))) {
            setMbrActiveExclusive(st.sector.data(), st.sector.size(), st.selected);
            st.dirty = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!sel.active);
        if (ImGui::Button(T_("Clear active"))) {
            setMbrActiveEntry(st.sector.data(), st.sector.size(), st.selected, false);
            st.dirty = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (mbrHiddenTypeOf(sel.type) != 0) {
            if (ImGui::Button(T_("Hide"))) {
                setMbrEntryType(st.sector.data(), st.sector.size(), st.selected,
                                mbrHiddenTypeOf(sel.type));
                st.dirty = true;
            }
            ImGui::SameLine();
        }
        if (mbrVisibleTypeOf(sel.type) != 0) {
            if (ImGui::Button(T_("Unhide"))) {
                setMbrEntryType(st.sector.data(), st.sector.size(), st.selected,
                                mbrVisibleTypeOf(sel.type));
                st.dirty = true;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(T_("Delete"))) {
            clearMbrEntry(st.sector.data(), st.sector.size(), st.selected);
            st.dirty = true;
            st.selected = -1;
        }
        ImGui::EndDisabled();

        // --- type combo -------------------------------------------------------------------
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        ImGui::BeginDisabled(!entryEditable);
        if (ImGui::BeginCombo("##parttype", T_(kTypes[st.typeIndex].label))) {
            for (int i = 0; i < (int)std::size(kTypes); ++i) {
                if (ImGui::Selectable(T_(kTypes[i].label), i == st.typeIndex)) {
                    st.typeIndex = i;
                    if (entryEditable) {
                        setMbrEntryType(st.sector.data(), st.sector.size(), st.selected,
                                        kTypes[i].type);
                        st.dirty = true;
                    }
                }
                if (i == st.typeIndex) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        // --- write / revert ------------------------------------------------------------------
        ImGui::Spacing();
        ImGui::BeginDisabled(!st.dirty || st.gpt);
        if (ImGui::Button(T_("Write to disk"))) {
            confirmAndWrite(app, st, st.sector);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!st.dirty);
        if (ImGui::Button(T_("Revert"))) {
            std::string err;
            if (readTarget(app, st, &err)) {
                st.error.clear();
            } else {
                st.error = err;
            }
        }
        ImGui::EndDisabled();
    }

    // --- status ---------------------------------------------------------------------------
    if (!st.error.empty()) {
        ImGui::TextColored(kColError, "%s", st.error.c_str());
    } else if (!st.info.empty()) {
        ImGui::TextColored(kColOk, "%s", st.info.c_str());
    }
}

} // namespace bootroll
