#include "core/bcd/Utf16.h"

#include <cstring>

namespace bootroll {

void utf8ToWide(const std::string& s, std::vector<uint16_t>& out)
{
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        uint8_t b = uint8_t(s[i]);
        uint32_t c;
        int len;
        if (b < 0x80) {
            c = b;
            len = 1;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size()) {
            c = b & 0x1F;
            len = 2;
        } else if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
            c = b & 0x0F;
            len = 3;
        } else if ((b & 0xF8) == 0xF0 && i + 3 < s.size()) {
            c = b & 0x07;
            len = 4;
        } else {
            c = b; // invalid byte -> latin-1 fallback
            len = 1;
        }
        i += size_t(len);
        if (len >= 2) {
            for (int k = 1; k < len; ++k)
                c = (c << 6) | (uint8_t(s[i - len + k]) & 0x3F);
        }
        if (c < 0x10000) {
            out.push_back(uint16_t(c));
        } else {
            c -= 0x10000;
            out.push_back(uint16_t(0xD800 + (c >> 10)));
            out.push_back(uint16_t(0xDC00 + (c & 0x3FF)));
        }
    }
}

std::string utf16ToUtf8(const uint16_t* w, size_t count)
{
    std::string out;
    out.reserve(count * 3 / 2);
    for (size_t i = 0; i < count; ++i) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) { // surrogate pair
            uint32_t lo = w[i + 1];
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                continue;
            }
        }
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (c >> 18)));
            out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::string utf16leToUtf8(const uint8_t* bytes, size_t byteCount)
{
    // Skip a BOM when present.
    if (byteCount >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        bytes += 2;
        byteCount -= 2;
    }
    std::vector<uint16_t> units;
    units.reserve(byteCount / 2);
    for (size_t i = 0; i + 1 < byteCount; i += 2) // byte-wise: alignment-safe
        units.push_back(uint16_t(bytes[i]) | uint16_t(bytes[i + 1]) << 8);
    return utf16ToUtf8(units.data(), units.size());
}

std::vector<uint8_t> utf8ToUtf16le(const std::string& s)
{
    std::vector<uint16_t> w;
    utf8ToWide(s, w);
    std::vector<uint8_t> out(w.size() * 2, 0);
    memcpy(out.data(), w.data(), out.size());
    return out;
}

std::vector<std::string> regMultiFromBytes(const uint8_t* data, size_t byteCount)
{
    std::vector<std::string> out;
    std::vector<uint16_t> units;
    units.reserve(byteCount / 2);
    for (size_t i = 0; i + 1 < byteCount; i += 2)
        units.push_back(uint16_t(data[i]) | uint16_t(data[i + 1]) << 8);
    std::vector<std::string> parts;
    std::string cur;
    std::vector<uint16_t> seg;
    for (uint16_t u : units) {
        if (u == 0) {
            parts.push_back(utf16ToUtf8(seg.data(), seg.size()));
            seg.clear();
        } else {
            seg.push_back(u);
        }
    }
    // A missing trailing NUL still yields the last segment.
    if (!seg.empty())
        parts.push_back(utf16ToUtf8(seg.data(), seg.size()));
    // REG_MULTI_SZ ends with an empty terminator; drop exactly one trailing empty.
    if (!parts.empty() && parts.back().empty())
        parts.pop_back();
    return parts;
}

std::vector<uint8_t> regMultiToBytes(const std::vector<std::string>& list)
{
    std::vector<uint16_t> units;
    for (const std::string& s : list) {
        std::vector<uint16_t> w;
        utf8ToWide(s, w);
        units.insert(units.end(), w.begin(), w.end());
        units.push_back(0);
    }
    units.push_back(0); // final double-NUL terminator
    std::vector<uint8_t> out(units.size() * 2, 0);
    memcpy(out.data(), units.data(), out.size());
    return out;
}

} // namespace bootroll
