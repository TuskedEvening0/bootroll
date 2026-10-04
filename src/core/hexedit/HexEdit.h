#pragma once
// Cursor + nibble editing model for the hex view (M5 sector editor).
// Pure logic, no UI dependency, so the edit state machine is unit-testable.
#include <cstdint>
#include <vector>

namespace bootroll::hexedit {

struct State {
    int selected = -1;   // byte index into the buffer, -1 = nothing selected
    int nibbleStage = 0; // 0 = next digit fills the high nibble
};

bool isHexDigit(char c);

// Value of a hex digit (0-15), or -1 when c is not a hex digit.
// Both letter cases are accepted.
int hexDigitValue(char c);

// Apply one hex digit to the selected byte. The first digit replaces the
// high nibble (keeping the low nibble), the second replaces the low nibble
// and advances the cursor by one byte. Returns false (and changes nothing)
// when nothing is selected or the character is not a hex digit.
bool applyDigit(std::vector<uint8_t>& data, State& s, char digit);

// Move the cursor by dx/dy cells (one row = 16 bytes), clamped to the
// buffer. Moving resets the nibble stage. Returns false when the buffer is
// empty or nothing is selected.
bool moveCursor(State& s, size_t size, int dx, int dy);

} // namespace bootroll::hexedit
