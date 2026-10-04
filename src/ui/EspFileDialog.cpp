#include "ui/EspFileDialog.h"

#include "app/App.h"
#include "app/I18n.h"
#include "core/disk/DiskInfo.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace bootroll {

namespace {

constexpr const char* kEspTypeGuid = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b";

std::string lowerCopy(std::string s)
{
    for (char& c : s) {
        c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

// A partition is worth listing when it may hold a FAT file system: an ESP
// (letter-less on Windows), a mounted FAT volume, or an unmountable volume.
bool mayBeFat(const PartitionInfo& p)
{
    if (lowerCopy(p.gptTypeGuid) == kEspTypeGuid) {
        return true;
    }
    const std::string fs = lowerCopy(p.fsName);
    if (fs.find("fat") != std::string::npos) {
        return true;
    }
    return fs.empty();
}

ImVec4 colorValue() { return ImVec4(0.6f, 0.85f, 1.0f, 1.0f); }
ImVec4 colorError() { return ImVec4(1.0f, 0.45f, 0.45f, 1.0f); }
ImVec4 colorDir() { return ImVec4(1.0f, 0.85f, 0.3f, 1.0f); }

std::string joinDir(const std::string& dir, const std::string& name)
{
    if (dir.empty()) {
        return "\\" + name;
    }
    return dir + "\\" + name;
}

std::string parentDir(const std::string& dir)
{
    const size_t p = dir.find_last_of('\\');
    return p == std::string::npos ? std::string() : dir.substr(0, p);
}

} // namespace

void EspFileDialog::open(App& app, int diskIdx, int partIdx)
{
    m_open = true;
    m_popupQueued = true;
    m_selected = -1;
    m_curDir.clear();
    m_diskIdx = diskIdx;
    m_partIdx = partIdx;
    ensureChoices(app);
    openVolume(app);
}

bool EspFileDialog::ensureChoices(App& app)
{
    m_choices.clear();
    const std::vector<DiskInfo>& disks = app.disks();
    for (size_t di = 0; di < disks.size(); ++di) {
        const DiskInfo& d = disks[di];
        for (size_t pi = 0; pi < d.partitions.size(); ++pi) {
            const PartitionInfo& p = d.partitions[pi];
            if (!mayBeFat(p)) {
                continue;
            }
            Choice c;
            c.diskIdx = int(di);
            c.partIdx = int(pi);
            c.label = diskShortName(d) + " / " + T_("Partition") + " " +
                      std::to_string(p.number);
            if (lowerCopy(p.gptTypeGuid) == kEspTypeGuid) {
                c.label += " (ESP)";
            }
            if (!p.fsName.empty()) {
                c.label += " - " + p.fsName;
            }
            c.label += " - " + formatSize(p.sizeBytes);
            m_choices.push_back(std::move(c));
        }
    }
    // Keep the requested partition selected when it is listed.
    for (size_t i = 0; i < m_choices.size(); ++i) {
        if (m_choices[i].diskIdx == m_diskIdx && m_choices[i].partIdx == m_partIdx) {
            return true;
        }
    }
    if (!m_choices.empty()) {
        m_diskIdx = m_choices[0].diskIdx;
        m_partIdx = m_choices[0].partIdx;
        return true;
    }
    m_diskIdx = m_partIdx = -1;
    return false;
}

void EspFileDialog::openVolume(App& app)
{
    m_volumeOk = false;
    m_volumeError.clear();
    m_listing.clear();
    m_selected = -1;
    if (m_diskIdx < 0 || m_diskIdx >= int(app.disks().size())) {
        m_volumeError = T_("No FAT-capable partition available.");
        return;
    }
    const DiskInfo& d = app.disks()[size_t(m_diskIdx)];
    if (m_partIdx < 0 || m_partIdx >= int(d.partitions.size())) {
        m_volumeError = T_("No FAT-capable partition available.");
        return;
    }
    const PartitionInfo& p = d.partitions[size_t(m_partIdx)];
    if (d.sectorSize == 0 || p.offsetBytes % d.sectorSize != 0) {
        m_volumeError = T_("This partition is not sector aligned.");
        return;
    }

    const uint32_t diskNumber = d.number;
    const uint64_t base = p.offsetBytes;
    const uint32_t diskSector = d.sectorSize;
    FatVolumeReader reader = [this, &app, diskNumber, base,
                              diskSector](uint64_t off, uint32_t bytes,
                                          uint8_t* out) -> bool {
        // Raw disk I/O needs disk-sector aligned ranges; widen and slice.
        const uint64_t absStart = base + off;
        const uint64_t aligned = absStart - absStart % diskSector;
        const uint64_t absEnd = absStart + bytes;
        const uint64_t alignedEnd = absEnd + (diskSector - absEnd % diskSector) % diskSector;
        std::vector<uint8_t> buf(size_t(alignedEnd - aligned));
        try {
            app.diskAccess()->readSectors(diskNumber, aligned, buf.data(), buf.size());
        } catch (const std::exception&) {
            return false;
        }
        std::memcpy(out, buf.data() + (absStart - aligned), bytes);
        return true;
    };

    std::string err;
    if (!m_volume.open(std::move(reader), &err)) {
        m_volumeError = std::string(T_("Cannot open the file system on this partition.")) +
                        "\n" + err;
        return;
    }
    m_volumeOk = true;
    reload();
}

void EspFileDialog::reload()
{
    m_listing.clear();
    m_selected = -1;
    if (!m_volumeOk) {
        return;
    }
    std::string err;
    if (!m_volume.listDir(m_curDir, &m_listing, &err)) {
        m_volumeError = std::string(T_("Cannot read the directory.")) + "\n" + err;
        m_volumeOk = false;
        return;
    }
    // Directories first, then files, each alphabetical (case-insensitive).
    std::stable_sort(m_listing.begin(), m_listing.end(),
                     [](const FatDirEntry& a, const FatDirEntry& b) {
                         if (a.isDir != b.isDir) {
                             return a.isDir > b.isDir;
                         }
                         return lowerCopy(a.name) < lowerCopy(b.name);
                     });
}

bool EspFileDialog::draw(App& app, std::string* outPath)
{
    if (!m_open) {
        return false;
    }
    // Stand-alone top-level window (movable, with a close box) like
    // DiskGenius's file browser, not a modal pinned inside the main window.
    if (m_popupQueued) {
        m_popupQueued = false;
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(660, 430), ImGuiCond_FirstUseEver);
    }

    bool confirmed = false;
    if (!ImGui::Begin(T_("Select a file on the ESP"), &m_open,
                      ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End(); // collapsed: still must pair Begin with End
        return false;
    }
    if (!m_open) {
        ImGui::End(); // closed via the title bar X this frame
        return false;
    }

    // Partition selector.
    if (!m_choices.empty()) {
        std::string joined;
        for (const Choice& c : m_choices) {
            joined += c.label;
            joined += '\0';
        }
        joined += '\0';
        int sel = -1;
        for (size_t i = 0; i < m_choices.size(); ++i) {
            if (m_choices[i].diskIdx == m_diskIdx && m_choices[i].partIdx == m_partIdx) {
                sel = int(i);
                break;
            }
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo(T_("Partition"), &sel, joined.c_str()) && sel >= 0 &&
            size_t(sel) < m_choices.size()) {
            m_diskIdx = m_choices[size_t(sel)].diskIdx;
            m_partIdx = m_choices[size_t(sel)].partIdx;
            m_curDir.clear();
            openVolume(app);
        }
    } else {
        ImGui::TextColored(colorError(), "%s", T_("No FAT-capable partition available."));
    }

    if (m_volumeOk) {
        // Toolbar: up + refresh + current path.
        const bool atRoot = m_curDir.empty();
        ImGui::BeginDisabled(atRoot);
        if (ImGui::Button(T_("Up"))) {
            m_curDir = parentDir(m_curDir);
            reload();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(T_("Refresh"))) {
            openVolume(app);
        }
        ImGui::SameLine();
        ImGui::TextColored(colorValue(), "%s",
                           (std::string("\\") + m_curDir).c_str());
        ImGui::Separator();

        // File list.
        ImGui::BeginChild("##espfiles", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()),
                          ImGuiChildFlags_Borders);
        ImGui::Columns(4, "##espcols");
        ImGui::SetColumnWidth(0, 300);
        ImGui::SetColumnWidth(1, 90);
        ImGui::SetColumnWidth(2, 90);
        ImGui::TextUnformatted(T_("Name"));
        ImGui::NextColumn();
        ImGui::TextUnformatted(T_("Type"));
        ImGui::NextColumn();
        ImGui::TextUnformatted(T_("Size"));
        ImGui::NextColumn();
        ImGui::TextUnformatted(T_("Modified"));
        ImGui::NextColumn();
        ImGui::Separator();

        for (int i = 0; i < int(m_listing.size()); ++i) {
            const FatDirEntry& e = m_listing[size_t(i)];
            if (e.isVolumeLabel) {
                continue;
            }
            ImGui::PushID(i);
            const bool isSelected = m_selected == i;
            if (e.isDir) {
                ImGui::PushStyleColor(ImGuiCol_Text, colorDir());
            }
            if (ImGui::Selectable(e.name.c_str(), isSelected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                m_selected = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (e.isDir) {
                        m_curDir = joinDir(m_curDir, e.name);
                        reload();
                        ImGui::PopID();
                        if (e.isDir) {
                            ImGui::PopStyleColor();
                        }
                        break;
                    }
                    *outPath = joinDir(m_curDir, e.name);
                    confirmed = true;
                }
            }
            if (e.isDir) {
                ImGui::PopStyleColor();
            }
            ImGui::NextColumn();
            ImGui::TextUnformatted(e.isDir ? T_("<DIR>") : "");
            ImGui::NextColumn();
            char sizeBuf[32];
            if (!e.isDir) {
                std::snprintf(sizeBuf, sizeof(sizeBuf), "%llu",
                              (unsigned long long)e.sizeBytes);
                ImGui::TextUnformatted(sizeBuf);
            }
            ImGui::NextColumn();
            if (e.mtime > 0) {
                const std::time_t t = std::time_t(e.mtime);
                std::tm tmv {};
                localtime_s(&tmv, &t);
                char timeBuf[32];
                std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", &tmv);
                ImGui::TextUnformatted(timeBuf);
            }
            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);
        ImGui::EndChild();
    } else if (!m_volumeError.empty()) {
        ImGui::TextColored(colorError(), "%s", m_volumeError.c_str());
        if (ImGui::Button(T_("Retry"))) {
            openVolume(app);
        }
    }

    ImGui::Separator();
    // Footer: selected path + OK/Cancel.
    if (m_volumeOk && m_selected >= 0 && m_selected < int(m_listing.size())) {
        const FatDirEntry& e = m_listing[size_t(m_selected)];
        if (!e.isDir && !e.isVolumeLabel) {
            ImGui::TextColored(colorValue(), "%s",
                               joinDir(m_curDir, e.name).c_str());
        }
    }
    const bool canConfirm =
        m_volumeOk && m_selected >= 0 && m_selected < int(m_listing.size()) &&
        !m_listing[size_t(m_selected)].isDir &&
        !m_listing[size_t(m_selected)].isVolumeLabel;
    ImGui::BeginDisabled(!canConfirm);
    if (ImGui::Button(T_("OK")) && canConfirm) {
        const FatDirEntry& e = m_listing[size_t(m_selected)];
        *outPath = joinDir(m_curDir, e.name);
        confirmed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T_("Cancel"))) {
        m_open = false;
    }

    if (confirmed) {
        m_open = false;
    }
    ImGui::End();
    return confirmed;
}

} // namespace bootroll
