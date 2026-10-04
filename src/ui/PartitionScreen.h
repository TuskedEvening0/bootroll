#pragma once
// Partition manager page (M4): view and edit the MBR partition table of a
// physical disk or a raw image file (VHD attached disks work too). Supports
// the BOOTICE-style flags: activate (exclusive), hide/unhide (type byte
// mapping), delete and change type. GPT disks are shown read-only.
// Edits stay in memory (dirty flag) until written; every write first creates
// an automatic backup of sector 0, then asks for confirmation.
namespace bootroll {

class App;

struct PartitionScreen {
    static void drawBody(App& app);
};

} // namespace bootroll
