#include "ui/MainScreen.h"
#include "ui/ElevateHint.h"
#include "app/App.h"
#include "app/I18n.h"
#include "app/Settings.h"
#include "core/disk/DiskInfo.h"
#include "imgui.h"
#include "ui/widgets/DiskPicker.h"

#include <cstdio>
#include <iterator>

namespace bootroll {

namespace {

void drawDiskSummary(const DiskInfo& d)
{
    char line[256];
    std::snprintf(line, sizeof(line), "%s: %s    %s: %s    %s: %u B",
                  T_("Model"), d.model.c_str(),
                  T_("Bus"), d.busType.c_str(),
                  T_("Sector size"), d.sectorSize);
    ImGui::TextUnformatted(line);
    std::snprintf(line, sizeof(line), "%s: %s    %s: %s%s",
                  T_("Size"), formatSize(d.totalBytes).c_str(),
                  T_("Partition style"),
                  d.style == PartitionStyle::Gpt ? T_("GPT")
                      : d.style == PartitionStyle::Mbr ? T_("MBR") : T_("Unknown"),
                  d.isVhd ? "  [VHD]" : "");
    ImGui::TextUnformatted(line);
    ImGui::Spacing();
}

void drawPartitionTable(const DiskInfo& d)
{
    static ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##partitions", 8, flags)) {
        return;
    }
    ImGui::TableSetupColumn(T_("No."), ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn(T_("Active"), ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn(T_("Hidden"), ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn(T_("Type"));
    ImGui::TableSetupColumn(T_("Size"), ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn(T_("Letter"), ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn(T_("Label"));
    ImGui::TableSetupColumn(T_("Filesystem"));
    ImGui::TableHeadersRow();

    for (const auto& p : d.partitions) {
        ImGui::TableNextRow();
        int col = 0;
        auto next = [&](const char* text) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::TextUnformatted(text);
        };

        char num[16], type[96], size[32];
        std::snprintf(num, sizeof(num), "%u", p.number);
        next(num);

        next(p.bootIndicator ? "*" : "");
        next(p.hidden ? "*" : "");

        if (p.style == PartitionStyle::Mbr) {
            std::snprintf(type, sizeof(type), "0x%02X", p.typeCode);
        } else if (p.style == PartitionStyle::Gpt) {
            if (p.gptTypeGuid == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") {
                std::snprintf(type, sizeof(type), "ESP");
            } else if (p.gptTypeGuid == "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7") {
                std::snprintf(type, sizeof(type), "Basic data");
            } else {
                std::snprintf(type, sizeof(type), "%s", p.gptTypeGuid.c_str());
            }
        } else {
            std::snprintf(type, sizeof(type), "?");
        }
        next(type);

        std::snprintf(size, sizeof(size), "%s", formatSize(p.sizeBytes).c_str());
        next(size);
        next(p.driveLetter.c_str());
        next(p.label.c_str());
        next(p.fsName.c_str());
    }
    ImGui::EndTable();
}

struct TabEntry {
    const char* label;
    App::Page page;
};

} // namespace

void MainScreen::drawTopBar(App& app)
{
    ImGui::TextUnformatted(T_("Target disk:"));
    ImGui::SameLine();
    DiskPicker::draw(app, 420.0f, "hdr");

    ImGui::SameLine();
    ImGui::Spacing();

    // Language switch (also proves .po catalogs are loading).
    ImGui::SameLine();
    ImGui::TextUnformatted(T_("Language:"));
    ImGui::SameLine();
    static const char* langs[] = { "zh_CN", "en_US", "" };
    static const char* langNames[] = { "简体中文", "English", "System/msgid" };
    int cur = 0;
    const std::string& curLang = I18n::instance().language();
    if (curLang == "en_US") {
        cur = 1;
    } else if (curLang.empty()) {
        cur = 2;
    }
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##lang", langNames[cur])) {
        for (int i = 0; i < 3; ++i) {
            if (ImGui::Selectable(langNames[i], i == cur)) {
                I18n::instance().setLanguage(langs[i]);
                Settings::instance().language = langs[i];
            }
        }
        ImGui::EndCombo();
    }
}

void MainScreen::drawTabBar(App& app)
{
    // BOOTICE-style top tab bar: every major function is a tab instead of a
    // button grid on a home screen. The active tab drives App::Page, and
    // programmatic page changes (gotoPage) select the matching tab back.
    static const TabEntry kTabs[] = {
        { "Disk Info", App::Page::Main },
        { "BCD Edit", App::Page::Bcd },
        { "Process MBR", App::Page::Mbr },
        { "Process PBR", App::Page::Pbr },
        { "Grub4DOS", App::Page::Grub4dos },
        { "Partition Manager", App::Page::Partition },
        { "Sector Editor", App::Page::Sector },
        { "UEFI", App::Page::Uefi },
        { "About", App::Page::About },
    };
    static App::Page lastSynced = App::Page::Main;
    const bool externalJump = (app.page() != lastSynced);

    if (!ImGui::BeginTabBar("##maintabs", ImGuiTabBarFlags_None))
        return;
    for (const TabEntry& t : kTabs) {
        ImGuiTabItemFlags flags = (externalJump && app.page() == t.page)
                                      ? ImGuiTabItemFlags_SetSelected
                                      : ImGuiTabItemFlags_None;
        const bool selected = ImGui::BeginTabItem(T_(t.label), nullptr, flags);
        // A single click must switch pages. BeginTabItem() still returns false
        // on the click frame (the queued selection only lands next frame), and
        // IsItemActivated() has already latched by then, so detect the click
        // here instead: IsItemClicked fires on the mouse-down frame for the
        // hovered tab regardless of which tab is currently selected.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && app.page() != t.page)
            app.gotoPage(t.page);
        if (selected)
            ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    lastSynced = app.page();
}

void MainScreen::drawBody(App& app)
{
    elevate::maybeShowModal(app);
    elevate::drawBanner(app);

    const DiskInfo* disk = app.currentDisk();
    if (!disk) {
        ImGui::TextWrapped("%s", T_("No disk detected."));
        return;
    }
    drawDiskSummary(*disk);
    drawPartitionTable(*disk);
}

} // namespace bootroll
