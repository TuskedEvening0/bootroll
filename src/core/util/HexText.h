#pragma once
// Hex text <-> bytes helpers (BCD raw element editor, sector editor).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bootroll {

// Parse hex text into bytes. Accepts optional "0x" prefixes, separators
// (space / comma / underscore / newline) and contiguous hex; digits are
// paired left to right after skipping separators. An odd number of hex
// digits or a non-hex character fails.
bool hexTextToBytes(std::string_view text, std::vector<uint8_t>* out);

// Bytes -> lowercase hex text, two digits per byte.
// spaced=false produces a compact contiguous string.
std::string bytesToHexText(const uint8_t* data, size_t size, bool spaced = true);
inline std::string bytesToHexText(const std::vector<uint8_t>& v, bool spaced = true)
{
    return bytesToHexText(v.data(), v.size(), spaced);
}

} // namespace bootroll
