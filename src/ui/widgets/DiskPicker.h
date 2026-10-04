#pragma once
// Disk selection combo widget with refresh support.
namespace bootroll {

class App;

struct DiskPicker {
    // Draws the combo + refresh button. Returns true when selection changed.
    // idScope must be unique per call site: the header picker and a page
    // picker are visible at the same time, and identical inner IDs would
    // collide (ImGui "conflicting ID" error).
    static bool draw(App& app, float comboWidth, const char* idScope);
};

} // namespace bootroll
