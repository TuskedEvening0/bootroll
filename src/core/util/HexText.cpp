#include "core/util/HexText.h"

namespace bootroll {

namespace {

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool isSep(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' ||
           c == '_' || c == '-';
}

} // namespace

bool hexTextToBytes(std::string_view text, std::vector<uint8_t>* out)
{
    out->clear();
    size_t i = 0;
    while (i < text.size()) {
        if (isSep(text[i])) {
            ++i;
            continue;
        }
        if (text[i] == '0' && i + 1 < text.size() &&
            (text[i + 1] == 'x' || text[i + 1] == 'X')) {
            i += 2;
            continue; // "0x" prefix between pairs is fine
        }
        int hi = hexDigit(text[i]);
        if (hi < 0)
            return false;
        ++i;
        // Skip separators between the two digits of a byte pair.
        while (i < text.size() && isSep(text[i]))
            ++i;
        int lo = i < text.size() ? hexDigit(text[i]) : -1;
        if (lo < 0)
            return false; // odd digit count or truncated pair
        ++i;
        out->push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

std::string bytesToHexText(const uint8_t* data, size_t size, bool spaced)
{
    static const char* kDigits = "0123456789abcdef";
    std::string s;
    s.reserve(size * (spaced ? 3 : 2));
    for (size_t i = 0; i < size; ++i) {
        if (spaced && i)
            s += ' ';
        s += kDigits[data[i] >> 4];
        s += kDigits[data[i] & 0xF];
    }
    return s;
}

} // namespace bootroll
