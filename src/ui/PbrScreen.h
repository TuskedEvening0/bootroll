#pragma once
// PBR page (M3): inspect / backup / restore / install a partition boot record
// of a partition on the selected disk (VHDs included). Install sources: a
// reference volume (PBR extracted live, BPB geometry patched to the target)
// or a user-provided 512-byte boot sector file.
// Every destructive write first creates an automatic backup, then asks for
// confirmation (project safety rule).
namespace bootroll {

class App;

struct PbrScreen {
    static void drawBody(App& app);
};

} // namespace bootroll
