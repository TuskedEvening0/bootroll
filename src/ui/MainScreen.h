#pragma once
// Main page: target disk picker + function entries + disk/partition summary.
namespace bootroll {

class App;

struct MainScreen {
    // Top strip: disk selector, language switch. Drawn on every page.
    static void drawTopBar(App& app);
    // BOOTICE-style function tab bar (Disk Info / BCD / MBR / PBR / ...).
    static void drawTabBar(App& app);
    // "Disk Info" tab body: selected disk summary + partition table.
    static void drawBody(App& app);
};

} // namespace bootroll
