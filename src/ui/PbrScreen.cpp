#include "ui/PbrScreen.h"
#include "ui/ElevateHint.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/util/LocalTime.h"
#include "core/bootcode/BootCode.h"
#include "core/bootcode/PbrCode.h"
#include "ui/widgets/DiskPicker.h"
#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <cstring>
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

std::string toLower(std::string s)
{
    for (char& c : s) {
        c = char(std::tolower((unsigned char)c));
    }
    return s;
}

PbrKind kindForFsName(const std::string& fs)
{
    const std::string f = toLower(fs);
    if (f == "ntfs") return PbrKind::Ntfs;
    if (f == "fat32") return PbrKind::Fat32;
    if (f == "fat" || f == "fat16") return PbrKind::Fat16;
    if (f == "fat12") return PbrKind::Fat12;
    if (f == "exfat") return PbrKind::ExFat;
    return PbrKind::Unknown;
}

bool isPatchableKind(PbrKind k)
{
    switch (k) {
    case PbrKind::Ntfs:
    case PbrKind::Fat12:
    case PbrKind::Fat16:
    case PbrKind::Fat32:
    case PbrKind::ExFat:
        return true;
    default:
        return false;
    }
}

struct PbrUi {
    int partIndex = 0;            // index into currentDisk()->partitions
    bool loaded = false;          // cached target boot sector valid
    std::string triedKey;         // target signature of the last auto-read attempt
    std::vector<uint8_t> sector;  // cached target partition boot sector
    PbrKind kind = PbrKind::Unknown;

    int source = 0;               // 0 = reference volume, 1 = boot sector file,
                                  // 2 = bundled Grub4DOS grldr.pbr
    std::string volLetter;        // selected reference volume ("C:")
    std::vector<VolumeInfo> volumes;
    char srcFilePath[1024] = {};

    std::string error;
    std::string info;
};

PbrUi& ui()
{
    static PbrUi s;
    return s;
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

const VolumeInfo* selectedVolume(const PbrUi& st)
{
    for (const VolumeInfo& v : st.volumes) {
        if (v.driveLetter == st.volLetter) {
            return &v;
        }
    }
    return nullptr;
}

bool readTarget(App& app, PbrUi& st, std::string* err)
{
    const DiskInfo* d = app.currentDisk();
    if (!d) {
        *err = T_("No disk selected.");
        return false;
    }
    if (d->partitions.empty()) {
        *err = T_("This disk has no partitions.");
        return false;
    }
    if (st.partIndex < 0 || st.partIndex >= (int)d->partitions.size()) {
        st.partIndex = 0;
    }
    const PartitionInfo& p = d->partitions[st.partIndex];
    st.sector.assign(kSector, 0);
    try {
        app.diskAccess()->readSectors(d->number, p.offsetBytes, st.sector.data(),
                                      st.sector.size());
    } catch (const std::exception& e) {
        *err = e.what();
        return false;
    }
    st.kind = detectPbrKind(st.sector.data(), st.sector.size());
    st.loaded = true;
    return true;
}

bool writeStaged(App& app, PbrUi& st, const std::vector<uint8_t>& bytes, std::string* err)
{
    const DiskInfo* d = app.currentDisk();
    if (!d || st.partIndex < 0 || st.partIndex >= (int)d->partitions.size()) {
        *err = T_("No partition selected.");
        return false;
    }
    try {
        app.diskAccess()->writeSectors(d->number,
                                       d->partitions[st.partIndex].offsetBytes,
                                       bytes.data(), bytes.size());
        app.diskAccess()->flush(d->number);
    } catch (const std::exception& e) {
        *err = e.what();
        return false;
    }
    return true;
}

// Reads exactly n bytes from the partition start (used to back up everything
// a multi-sector install is about to overwrite).
bool readTargetBytes(App& app, PbrUi& st, size_t bytes, std::vector<uint8_t>* out,
                     std::string* err)
{
    const DiskInfo* d = app.currentDisk();
    if (!d || st.partIndex < 0 || st.partIndex >= (int)d->partitions.size()) {
        *err = T_("No partition selected.");
        return false;
    }
    out->assign(bytes, 0);
    try {
        app.diskAccess()->readSectors(d->number, d->partitions[st.partIndex].offsetBytes,
                                      out->data(), out->size());
    } catch (const std::exception& e) {
        *err = e.what();
        return false;
    }
    return true;
}

bool patchSource(std::vector<uint8_t>& src, PbrKind kind, uint64_t hidden,
                 uint64_t total, std::string* err)
{
    bool ok = false;
    switch (kind) {
    case PbrKind::Ntfs:
        ok = patchNtfsBpb(src.data(), src.size(), hidden, total);
        break;
    case PbrKind::Fat12:
    case PbrKind::Fat16:
    case PbrKind::Fat32:
        ok = patchFatBpb(src.data(), src.size(), uint32_t(hidden), uint32_t(total));
        break;
    case PbrKind::ExFat:
        ok = patchExFatBpb(src.data(), src.size(), hidden, total);
        break;
    default:
        *err = T_("Source is not a supported boot sector.");
        return false;
    }
    if (!ok) {
        *err = T_("Cannot patch the boot sector geometry.");
    }
    return ok;
}

// Automatic backup of everything the staged write replaces + native confirm;
// writes on OK. body carries the question (and geometry) lines.
void confirmAndWrite(App& app, PbrUi& st, std::vector<uint8_t> staged,
                     const std::string& body, bool restore)
{
    std::string err;
    std::vector<uint8_t> backup;
    if (!readTargetBytes(app, st, staged.size(), &backup, &err)) {
        st.error = err;
        return;
    }
    const DiskInfo* d = app.currentDisk();
    const PartitionInfo& p = d->partitions[st.partIndex];
    const std::string tag = "pbr_disk" + std::to_string(d->number) +
                            "_p" + std::to_string(p.number);
    std::string bpath;
    if (!writeAutoBackup(app, tag, backup.data(), backup.size(), &bpath, &err)) {
        st.error = err;
        return;
    }
    const std::string text = body + "\n\n" + T_("Automatic backup saved to") + ": " + bpath;
    if (app.platform()->confirmDialog(T_("Confirm write"), text)) {
        if (writeStaged(app, st, staged, &err)) {
            st.info = restore ? T_("Backup restored.") : T_("PBR installed.");
            st.error.clear();
            st.loaded = false;      // refresh the readout from disk next time
            st.triedKey.clear();
        } else {
            st.error = err;
        }
    }
}

void installPbr(App& app, PbrUi& st)
{
    std::string err;
    if (!readTarget(app, st, &err)) {
        st.error = err;
        return;
    }

    // Bundled Grub4DOS PBR: write grldr.pbr (11 sectors), keeping the
    // target's BPB so the loader can parse the volume it sits on.
    if (st.source == 2) {
        size_t blobSize = 0;
        const uint8_t* blob = grub4dosPbrBlob(&blobSize);
        if (!blob || blobSize < 512) {
            st.error = T_("Boot code not bundled in this build.");
            return;
        }
        std::vector<uint8_t> staged(blob, blob + blobSize);
        std::memcpy(staged.data() + 0x0B, st.sector.data() + 0x0B, 0x40 - 0x0B);
        char head[160];
        std::snprintf(
            head, sizeof(head),
            T_("Install this boot sector? The first %u sectors of the partition will be overwritten."),
            (unsigned)(staged.size() / 512));
        confirmAndWrite(app, st, std::move(staged), head, false);
        return;
    }

    // 1) fetch the source boot sector
    std::vector<uint8_t> src;
    if (st.source == 0) {
        const VolumeInfo* v = selectedVolume(st);
        if (!v) {
            st.error = T_("No reference volume selected.");
            return;
        }
        if (!app.platform()->readVolumeFirstSector(v->driveLetter, &src, &err)) {
            st.error = err;
            return;
        }
    } else {
        if (st.srcFilePath[0] == '\0') {
            st.error = T_("No file specified.");
            return;
        }
        std::ifstream f(st.srcFilePath, std::ios::binary);
        if (!f) {
            st.error = std::string(T_("Cannot open file: ")) + st.srcFilePath;
            return;
        }
        src.assign(kSector, 0);
        f.read(reinterpret_cast<char*>(src.data()), std::streamsize(kSector));
        if (f.gcount() < std::streamsize(kSector)) {
            st.error = T_("File is smaller than one sector.");
            return;
        }
    }

    const PbrKind srcKind = detectPbrKind(src.data(), src.size());
    if (!isPatchableKind(srcKind)) {
        st.error = T_("Source is not a supported boot sector.");
        return;
    }

    // 2) filesystem sanity gate: never write a boot sector of the wrong family
    const DiskInfo* d = app.currentDisk();
    const PartitionInfo& p = d->partitions[st.partIndex];
    const PbrKind fsKind = kindForFsName(p.fsName);
    const PbrKind tgtKind = isPatchableKind(st.kind) ? st.kind : fsKind;
    if (tgtKind != PbrKind::Unknown && tgtKind != srcKind) {
        st.error = std::string(T_("Source filesystem does not match the target partition (")) +
                   std::string(pbrKindLabel(tgtKind)) + " / " +
                   std::string(pbrKindLabel(srcKind)) + ").";
        return;
    }

    // 3) patch the geometry fields (hidden sectors / volume size)
    if (d->sectorSize == 0) {
        st.error = T_("Cannot determine the target geometry.");
        return;
    }
    const uint64_t hidden = p.offsetBytes / d->sectorSize;
    const uint64_t total = p.sizeBytes / d->sectorSize;
    if (!patchSource(src, srcKind, hidden, total, &err)) {
        st.error = err;
        return;
    }

    // 4) automatic backup + native confirmation
    char head[256];
    std::snprintf(head, sizeof(head), "%s\n%s: %llu   %s: %llu",
                  T_("Install this boot sector? The first sector of the partition will be overwritten."),
                  T_("Hidden sectors"), (unsigned long long)hidden,
                  T_("Total sectors"), (unsigned long long)total);
    confirmAndWrite(app, st, std::move(src), head, false);
}

} // namespace

void PbrScreen::drawBody(App& app)
{
    PbrUi& st = ui();
    elevate::maybeShowModal(app);
    elevate::drawBanner(app);
    const DiskInfo* disk = app.currentDisk();
    if (disk && st.partIndex >= (int)disk->partitions.size()) {
        st.partIndex = 0;
        st.loaded = false;
    }

    // --- target row -----------------------------------------------------------------
    ImGui::TextUnformatted(T_("Target:"));
    ImGui::SameLine();
    if (DiskPicker::draw(app, 360, "pbr")) {
        st.partIndex = 0;
        st.loaded = false;
    }

    if (!disk || disk->partitions.empty()) {
        ImGui::TextDisabled("%s", T_("This disk has no partitions."));
    } else {
        const std::string cur = partitionLabel(disk->partitions[st.partIndex]);
        ImGui::SetNextItemWidth(360);
        if (ImGui::BeginCombo("##pbrpart", cur.c_str())) {
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
        ImGui::SameLine();
        if (ImGui::Button(T_("Read boot sector"))) {
            std::string err;
            if (readTarget(app, st, &err)) {
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
        if (!st.loaded && st.triedKey != wantKey) {
            st.triedKey = wantKey;
            std::string err;
            if (readTarget(app, st, &err)) {
                st.error.clear();
            } else {
                st.loaded = false;
                st.error = err;
            }
        }
    }

    // --- current PBR ------------------------------------------------------------------
    ImGui::Spacing();
    if (st.loaded) {
        ImGui::TextUnformatted(T_("Current PBR:"));
        ImGui::SameLine();
        const std::string kind = T_(std::string(pbrKindLabel(st.kind)).c_str());
        ImGui::TextColored(kColValue, "%s", kind.c_str());
    } else {
        ImGui::TextDisabled("%s", T_("Select a partition to inspect its boot sector."));
    }

    // --- install ------------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(T_("Install PBR:"));
    if (ImGui::RadioButton(T_("From reference volume"), &st.source, 0)) {
        st.error.clear();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(T_("From file"), &st.source, 1)) {
        st.error.clear();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(T_("Grub4DOS (grldr.pbr)"), &st.source, 2)) {
        st.error.clear();
    }

    if (st.source == 0) {
        if (st.volumes.empty()) {
            st.volumes = app.platform()->enumerateVolumes();
        }
        std::string preview = T_("No reference volume selected.");
        if (const VolumeInfo* v = selectedVolume(st)) {
            preview = v->driveLetter;
            if (!v->fsName.empty()) {
                preview += "  [" + v->fsName + "]";
            }
            if (!v->label.empty()) {
                preview += "  \"" + v->label + "\"";
            }
        }
        ImGui::SetNextItemWidth(320);
        if (ImGui::BeginCombo("##pbrvol", preview.c_str())) {
            for (int i = 0; i < (int)st.volumes.size(); ++i) {
                const VolumeInfo& v = st.volumes[i];
                if (v.driveLetter.empty()) {
                    continue; // letter-less volumes cannot be opened by letter
                }
                std::string label = v.driveLetter;
                if (!v.fsName.empty()) {
                    label += "  [" + v.fsName + "]";
                }
                if (!v.label.empty()) {
                    label += "  \"" + v.label + "\"";
                }
                if (ImGui::Selectable(label.c_str(), v.driveLetter == st.volLetter)) {
                    st.volLetter = v.driveLetter;
                }
                if (v.driveLetter == st.volLetter) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(T_("Refresh"))) {
            st.volumes = app.platform()->enumerateVolumes();
        }
    } else if (st.source == 1) {
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##pbrsrcfile", st.srcFilePath, sizeof(st.srcFilePath));
        ImGui::SameLine();
        if (ImGui::Button("...##pbrsrcdlg")) {
            const std::string p = app.platform()->openFileDialog(
                T_("Open boot sector file"),
                std::string(T_("Boot sector files")) + "|*.bin|" + T_("All files") + "|*.*");
            if (!p.empty()) {
                std::snprintf(st.srcFilePath, sizeof(st.srcFilePath), "%s", p.c_str());
            }
        }
    }

    ImGui::BeginDisabled(!st.loaded || disk == nullptr || disk->partitions.empty());
    if (ImGui::Button(T_("Install"))) {
        installPbr(app, st);
    }
    ImGui::EndDisabled();

    // --- backup / restore ------------------------------------------------------------------
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
                std::string err;
                if (!readTarget(app, st, &err)) {
                    st.error = err;
                } else {
                    confirmAndWrite(app, st, std::move(buf),
                                    T_("Restore this backup? The first sector of the partition will be overwritten."),
                                    true);
                }
            }
        }
    }

    // --- status --------------------------------------------------------------------------------
    if (!st.error.empty()) {
        ImGui::TextColored(kColError, "%s", st.error.c_str());
    } else if (!st.info.empty()) {
        ImGui::TextColored(kColOk, "%s", st.info.c_str());
    }
}

} // namespace bootroll
