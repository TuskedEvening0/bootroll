#pragma once
// UEFI page (M7): manage firmware boot entries (Boot####) stored in NVRAM -
// list, reorder, add/edit/delete, BootNext, timeout, backup export/restore.
// Every NVRAM write first creates an automatic backup, then asks for a native
// confirmation dialog (project safety rule).
namespace bootroll {

class App;

struct UefiScreen {
    static void drawBody(App& app);
};

} // namespace bootroll
