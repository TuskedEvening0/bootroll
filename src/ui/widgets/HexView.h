#pragma once
// ImGui hex grid for the sector editor (M5). Draws the buffer with a
// monospace font, handles click-to-select and keyboard nibble editing
// (core/hexedit). Returns true when the buffer contents were modified.
#include "core/hexedit/HexEdit.h"
#include "imgui.h"

#include <vector>

namespace bootroll {

struct HexView {
    // data is shown and edited in place; size should ideally be a multiple
    // of 16. mono should be a fixed-pitch font (falls back to the current
    // font). Keyboard input is only consumed while no text field of the
    // window is active (io.WantTextInput is honored).
    static bool draw(std::vector<uint8_t>& data, hexedit::State& st, ImFont* mono);
};

} // namespace bootroll
