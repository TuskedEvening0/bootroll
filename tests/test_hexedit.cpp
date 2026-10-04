// Unit tests for the hex editor model (M5 sector editor core logic).
#include "core/hexedit/HexEdit.h"

#include "doctest.h"

namespace hexedit = bootroll::hexedit;
using hexedit::State;

TEST_CASE("hexDigitValue classifies hex digits")
{
    CHECK(hexedit::isHexDigit('0'));
    CHECK(hexedit::isHexDigit('9'));
    CHECK(hexedit::isHexDigit('a'));
    CHECK(hexedit::isHexDigit('f'));
    CHECK(hexedit::isHexDigit('A'));
    CHECK(hexedit::isHexDigit('F'));
    CHECK_FALSE(hexedit::isHexDigit('g'));
    CHECK_FALSE(hexedit::isHexDigit('/'));
    CHECK_FALSE(hexedit::isHexDigit(':'));
    CHECK_FALSE(hexedit::isHexDigit('\0'));

    CHECK(hexedit::hexDigitValue('0') == 0);
    CHECK(hexedit::hexDigitValue('9') == 9);
    CHECK(hexedit::hexDigitValue('a') == 10);
    CHECK(hexedit::hexDigitValue('f') == 15);
    CHECK(hexedit::hexDigitValue('C') == 12);
    CHECK(hexedit::hexDigitValue('x') == -1);
}

TEST_CASE("applyDigit requires a selection and a hex digit")
{
    std::vector<uint8_t> data { 0xAB, 0xCD };
    State s; // nothing selected

    CHECK_FALSE(hexedit::applyDigit(data, s, '5'));
    CHECK(data[0] == 0xAB);

    s.selected = 0;
    CHECK_FALSE(hexedit::applyDigit(data, s, 'g'));
    CHECK(data[0] == 0xAB);
    CHECK(s.nibbleStage == 0);
}

TEST_CASE("applyDigit fills the high nibble, then the low nibble, then advances")
{
    std::vector<uint8_t> data { 0xAB, 0xCD };
    State s;
    s.selected = 0;

    // First digit replaces the high nibble only.
    CHECK(hexedit::applyDigit(data, s, '3'));
    CHECK(data[0] == 0x3B);
    CHECK(s.nibbleStage == 1);
    CHECK(s.selected == 0);

    // Second digit replaces the low nibble and advances the cursor.
    CHECK(hexedit::applyDigit(data, s, 'f'));
    CHECK(data[0] == 0x3F);
    CHECK(s.nibbleStage == 0);
    CHECK(s.selected == 1);

    // The advanced cursor edits the next byte.
    CHECK(hexedit::applyDigit(data, s, '0'));
    CHECK(hexedit::applyDigit(data, s, '7'));
    CHECK(data[1] == 0x07);
}

TEST_CASE("applyDigit clamps the cursor at the last byte")
{
    std::vector<uint8_t> data { 0x11 };
    State s;
    s.selected = 0;

    CHECK(hexedit::applyDigit(data, s, '2'));
    CHECK(hexedit::applyDigit(data, s, '2'));
    CHECK(data[0] == 0x22);
    CHECK(s.selected == 0);
}

TEST_CASE("applyDigit accepts upper and lower case digits")
{
    std::vector<uint8_t> a { 0x00 }, b { 0x00 };
    State sa, sb;
    sa.selected = sb.selected = 0;

    CHECK(hexedit::applyDigit(a, sa, 'A'));
    CHECK(hexedit::applyDigit(b, sb, 'a'));
    CHECK(a[0] == b[0]);
    CHECK(a[0] == 0xA0);
}

TEST_CASE("moveCursor moves and clamps")
{
    std::vector<uint8_t> data(32, 0); // two rows of 16
    State s;
    s.selected = 0;

    CHECK_FALSE(hexedit::moveCursor(s, data.size(), 0, -1)); // clamped at top
    CHECK(s.selected == 0);
    CHECK_FALSE(hexedit::moveCursor(s, data.size(), -1, 0)); // clamped at left

    CHECK(hexedit::moveCursor(s, data.size(), 1, 0));
    CHECK(s.selected == 1);
    CHECK(hexedit::moveCursor(s, data.size(), 0, 1));
    CHECK(s.selected == 17);
    CHECK(hexedit::moveCursor(s, data.size(), -16, 0));
    CHECK(s.selected == 1);

    s.selected = static_cast<int>(data.size()) - 1;
    CHECK_FALSE(hexedit::moveCursor(s, data.size(), 1, 0)); // clamped at end
    CHECK_FALSE(hexedit::moveCursor(s, data.size(), 0, 1));
}

TEST_CASE("moveCursor resets the nibble stage")
{
    std::vector<uint8_t> data(16, 0);
    State s;
    s.selected = 0;
    s.nibbleStage = 1;

    CHECK(hexedit::moveCursor(s, data.size(), 1, 0));
    CHECK(s.nibbleStage == 0);
}

TEST_CASE("moveCursor rejects empty buffers and missing selections")
{
    std::vector<uint8_t> data { 0x00 };
    State s;

    CHECK_FALSE(hexedit::moveCursor(s, 0, 1, 0));
    CHECK_FALSE(hexedit::moveCursor(s, data.size(), 1, 0)); // nothing selected

    s.selected = 0;
    CHECK_FALSE(hexedit::moveCursor(s, data.size(), 0, 0)); // no movement
}
