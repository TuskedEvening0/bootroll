#pragma once
// MBR page (M3): inspect / backup / restore / install the master boot record
// of a physical disk or a raw image file. Installable code: bootroll's own
// NT6-style MBR plus open-source blobs (Grub4DOS/WEE/Syslinux). Plop is
// detected but not installable (its license does not allow bundling).
// Every destructive write first creates an automatic backup, then asks for
// confirmation (project safety rule).
namespace bootroll {

class App;

struct MbrScreen {
    static void drawBody(App& app);
};

} // namespace bootroll
