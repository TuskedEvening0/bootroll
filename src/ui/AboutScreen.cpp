#include "ui/AboutScreen.h"
#include "app/App.h"
#include "app/I18n.h"
#include "imgui.h"
#include "Version.h"

#include <iterator>

namespace bootroll {

// Embedded license texts (CMakeLists embed_binary) - the exe is a single
// portable file, so the license texts must travel inside it.
extern const unsigned char lic_gpl2[];
extern const size_t lic_gpl2_size;
extern const unsigned char lic_ofl[];
extern const size_t lic_ofl_size;
extern const unsigned char lic_imgui[];
extern const size_t lic_imgui_size;
extern const unsigned char lic_tg[];
extern const size_t lic_tg_size;

namespace {

struct LicItem {
    const char* name;          // fixed, not translated (official names)
    const unsigned char* data;
    size_t size;
};

const LicItem kLicenses[] = {
    { "GNU GPL v2 - GRUB4DOS / WEE / Syslinux", lic_gpl2,   lic_gpl2_size },
    { "SIL OFL 1.1 - Noto Sans SC",             lic_ofl,    lic_ofl_size },
    { "MIT - Dear ImGui",                       lic_imgui,  lic_imgui_size },
    { "Zlib - tinygettext",                     lic_tg,     lic_tg_size },
};

void drawLicenseViewer()
{
    static int current = 0;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(T_("Full license texts:"));
    ImGui::SetNextItemWidth(360);
    if (ImGui::BeginCombo("##license", kLicenses[current].name)) {
        for (int i = 0; i < (int)std::size(kLicenses); ++i) {
            if (ImGui::Selectable(kLicenses[i].name, i == current)) {
                current = i;
            }
            if (i == current) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::BeginChild("##licensetext", ImVec2(0.0f, ImGui::GetTextLineHeight() * 14.0f),
                      ImGuiChildFlags_Borders);
    const char* begin = reinterpret_cast<const char*>(kLicenses[current].data);
    ImGui::TextUnformatted(begin, begin + kLicenses[current].size);
    ImGui::EndChild();
}

} // namespace

void AboutScreen::draw(App& app)
{
    ImGui::Text("Bootroll %s", BOOTROLL_APP_VERSION);
    ImGui::Separator();
    ImGui::TextWrapped("%s",
        T_("A boot configuration utility for Windows, rewritten with Dear ImGui + C++."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s",
        T_("Behavioral spec follows BOOTICE v1.3.3.2 (by Pauly, www.ipauly.com). "
           "This is an independent clean-room reimplementation; no original code, "
           "data or resources from BOOTICE are included."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s",
        T_("Third-party components: Dear ImGui (MIT), tinygettext (Zlib), "
           "Noto Sans SC (SIL OFL 1.1), GRUB4DOS / WEE / Syslinux (GPLv2, "
           "unmodified prebuilt binaries). See resources/licenses/THIRD_PARTY.md."));
    drawLicenseViewer();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("%s: %s", T_("Firmware"),
        app.platform()->firmwareType() == FirmwareType::Uefi ? "UEFI"
            : app.platform()->firmwareType() == FirmwareType::Bios ? "BIOS" : T_("Unknown"));
    ImGui::Text("%s: %s", T_("Elevated"),
        app.platform()->isElevated() ? T_("Yes") : T_("No"));
}

} // namespace bootroll
