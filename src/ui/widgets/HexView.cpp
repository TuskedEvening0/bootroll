#include "ui/widgets/HexView.h"

#include <cstdio>

namespace bootroll {

bool HexView::draw(std::vector<uint8_t>& data, hexedit::State& st, ImFont* mono)
{
    ImGuiIO& io = ImGui::GetIO();
    bool changed = false;

    if (mono) {
        ImGui::PushFont(mono);
    }

    const float charW = ImGui::CalcTextSize("0").x;
    const float lineH = ImGui::GetTextLineHeight();
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const float cellW = 2.0f * charW + 2.0f; // two hex digits + padding
    const float pitch = 3.0f * charW + 2.0f; // cell + one space
    const float groupGap = 1.0f * charW;     // extra gap after every 8 bytes
    const float offW = 5.0f * charW;         // "0123" offset column + gap
    const float hexStart = ImGui::GetCursorPosX() + offW;
    const float asciiX = hexStart + 17.0f * pitch + groupGap;

    const int rows = static_cast<int>((data.size() + 15) / 16);

    ImGuiListClipper clipper;
    clipper.Begin(rows, rowH);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            char off[8];
            std::snprintf(off, sizeof(off), "%04X", r * 16);
            ImGui::TextUnformatted(off);

            int count = static_cast<int>(data.size()) - r * 16;
            if (count > 16) {
                count = 16;
            }

            for (int c = 0; c < count; ++c) {
                const int idx = r * 16 + c;
                const float x =
                    hexStart + c * pitch + (c >= 8 ? groupGap : 0.0f);
                ImGui::SameLine(x);
                char cell[4];
                std::snprintf(cell, sizeof(cell), "%02X", data[idx]);
                ImGui::PushID(idx);
                if (ImGui::Selectable(cell, st.selected == idx,
                                      ImGuiSelectableFlags_None,
                                      ImVec2(cellW, lineH))) {
                    st.selected = idx;
                    st.nibbleStage = 0;
                }
                ImGui::PopID();
            }

            char ascii[17];
            for (int c = 0; c < count; ++c) {
                const uint8_t b = data[r * 16 + c];
                ascii[c] = (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
            }
            ImGui::SameLine(asciiX);
            ImGui::TextUnformatted(ascii, ascii + count);
        }
    }

    // Keyboard editing: only while no text field anywhere wants the keys.
    if (st.selected >= 0 && !io.WantTextInput &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        for (const ImWchar c : io.InputQueueCharacters) {
            if (c < 128 && hexedit::applyDigit(data, st, static_cast<char>(c))) {
                changed = true;
            }
        }
        struct Nav {
            ImGuiKey key;
            int dx, dy;
        };
        static const Nav kNav[] = {
            { ImGuiKey_LeftArrow, -1, 0 }, { ImGuiKey_RightArrow, 1, 0 },
            { ImGuiKey_UpArrow, 0, -1 },   { ImGuiKey_DownArrow, 0, 1 },
        };
        for (const Nav& n : kNav) {
            if (ImGui::IsKeyPressed(n.key, true)) {
                hexedit::moveCursor(st, data.size(), n.dx, n.dy);
            }
        }
    }

    if (mono) {
        ImGui::PopFont();
    }
    return changed;
}

} // namespace bootroll
