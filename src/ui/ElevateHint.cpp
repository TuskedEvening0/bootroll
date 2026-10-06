#include "ui/ElevateHint.h"

#include "app/App.h"
#include "app/I18n.h"
#include "imgui.h"

#include <set>

namespace bootroll {
namespace elevate {

namespace {

const ImVec4 kColWarn = ImVec4(0.95f, 0.80f, 0.30f, 1.0f);
const ImVec4 kColError = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);

// Restart through the platform's privilege mechanism (pkexec / UAC). On
// success the elevated replacement instance takes over and the UI shuts down.
void attemptRestart(App& app, bool* declined)
{
    if (app.platform()->restartElevated("")) {
        app.requestExit(); // elevated instance takes over
        return;
    }
    *declined = true;
}

// Button + inline declined feedback, shared by banner and modal.
void drawAction(App& app, bool* declined)
{
    if (ImGui::Button(T_(app.platform()->elevateActionMsgId()))) {
        attemptRestart(app, declined);
    }
    if (*declined) {
        ImGui::SameLine();
        ImGui::TextColored(kColError, "%s",
                           T_(app.platform()->elevateDeclinedMsgId()));
    }
}

} // namespace

void drawBanner(App& app)
{
    if (app.platform()->isElevated()) {
        return;
    }
#ifdef _WIN32
    ImGui::TextColored(kColWarn, "%s", T_(
        "Not running as administrator: disk contents cannot be accessed."));
#else
    ImGui::TextColored(kColWarn, "%s", T_(
        "Not running as root: disk contents cannot be accessed."));
    ImGui::TextDisabled("%s", T_("You can also run this program manually with sudo."));
#endif
    static bool declined = false; // per-process; cleared never (banner stays)
    drawAction(app, &declined);
    ImGui::Separator();
}

void maybeShowModal(App& app)
{
    if (app.platform()->isElevated()) {
        return;
    }
    // One prompt per page per process; afterwards the banner carries the hint.
    static std::set<App::Page> prompted;
    if (!prompted.insert(app.page()).second) {
        return;
    }

#ifdef _WIN32
    const char* title = "Administrator privileges required";
    const char* body =
        "This page needs administrator access to disk devices.";
#else
    const char* title = "Root privileges required";
    const char* body =
        "This page needs access to disk devices, which requires root privileges.";
#endif

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    // T_()/titles: ImGui copies popup and window names internally, so passing
    // the translated temporary is safe (same pattern as BeginTabItem).
    ImGui::OpenPopup(T_(title));
    if (ImGui::BeginPopupModal(T_(title), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", T_(body));
#ifndef _WIN32
        ImGui::TextDisabled("%s", T_("You can also run this program manually with sudo."));
#endif
        static bool declined = false;
        drawAction(app, &declined);
        ImGui::SameLine();
        if (ImGui::Button(T_("Continue without elevation"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace elevate
} // namespace bootroll
