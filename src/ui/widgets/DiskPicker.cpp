#include "ui/widgets/DiskPicker.h"
#include "app/App.h"
#include "app/I18n.h"
#include "core/disk/DiskInfo.h"
#include "imgui.h"

#include <cstdio>
#include <string>

namespace bootroll {

namespace {

std::string diskLabel(const DiskInfo& d)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Disk %u", d.number);
    std::string label = buf;
    if (!d.model.empty()) {
        label += "  " + d.model;
    }
    label += "  [" + formatSize(d.totalBytes) + "]";
    if (d.isVhd) {
        label += "  [VHD]";
    }
    return label;
}

} // namespace

bool DiskPicker::draw(App& app, float comboWidth, const char* idScope)
{
    bool changed = false;

    ImGui::PushID(idScope);
    ImGui::SetNextItemWidth(comboWidth);
    const int selected = app.selectedDisk();
    const std::string preview = (selected >= 0 && selected < (int)app.disks().size())
                                    ? diskLabel(app.disks()[selected])
                                    : std::string(T_("No disk detected."));
    if (ImGui::BeginCombo("##diskpicker", preview.c_str())) {
        for (int i = 0; i < (int)app.disks().size(); ++i) {
            const std::string label = diskLabel(app.disks()[i]);
            bool isSelected = (i == selected);
            if (ImGui::Selectable(label.c_str(), isSelected)) {
                app.selectDisk(i);
                changed = true;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button(T_("Refresh"))) {
        app.refreshDisks();
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

} // namespace bootroll
