#include "core/hexedit/HexEdit.h"

namespace bootroll::hexedit {

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int hexDigitValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool applyDigit(std::vector<uint8_t>& data, State& s, char digit)
{
    const int v = hexDigitValue(digit);
    if (v < 0 || s.selected < 0 || s.selected >= static_cast<int>(data.size())) {
        return false;
    }
    uint8_t& b = data[static_cast<size_t>(s.selected)];
    if (s.nibbleStage == 0) {
        b = static_cast<uint8_t>((b & 0x0F) | (v << 4));
        s.nibbleStage = 1;
    } else {
        b = static_cast<uint8_t>((b & 0xF0) | v);
        s.nibbleStage = 0;
        if (s.selected + 1 < static_cast<int>(data.size())) {
            ++s.selected;
        }
    }
    return true;
}

bool moveCursor(State& s, size_t size, int dx, int dy)
{
    if (size == 0 || s.selected < 0) {
        return false;
    }
    const int last = static_cast<int>(size) - 1;
    int next = s.selected + dx + dy * 16;
    if (next < 0) {
        next = 0;
    }
    if (next > last) {
        next = last;
    }
    if (next == s.selected) {
        return false;
    }
    s.selected = next;
    s.nibbleStage = 0;
    return true;
}

} // namespace bootroll::hexedit
