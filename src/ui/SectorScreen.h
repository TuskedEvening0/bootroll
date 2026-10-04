#pragma once
// Sector editor page (M5): hex view of one sector read from a physical disk
// or a raw image file (512-byte pages), with goto/prev/next navigation and
// in-place byte editing. Writes follow the shared safety flow: automatic
// backup of the target sector first, then an explicit confirmation. The
// editor refuses to navigate away while changes are unwritten.
namespace bootroll {

class App;

struct SectorScreen {
    static void drawBody(App& app);
};

} // namespace bootroll
