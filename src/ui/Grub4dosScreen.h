#pragma once
// Grub4DOS page (M4): install grldr.mbr into the MBR or grldr.pbr into a
// partition, write the bundled GRLDR boot file to a volume root, and edit the
// menu.lst that grldr loads from the volume root.
// Every destructive write first creates an automatic backup, then asks for
// confirmation (project safety rule).
namespace bootroll {

class App;

struct Grub4dosScreen {
    static void drawBody(App& app);
};

} // namespace bootroll
