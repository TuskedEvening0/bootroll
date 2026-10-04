#pragma once
// Shared UTF conversion helpers (portable, no OS dependency).
// REG_SZ/MULTI_SZ payloads in real BCD hives are UTF-16LE — BCD strings must
// go through these before hitting the UI.
#include <cstdint>
#include <string>
#include <vector>

namespace bootroll {

// UTF-8 -> UTF-16 code units (no BOM, no terminator).
void utf8ToWide(const std::string& s, std::vector<uint16_t>& out);

// UTF-16 code units -> UTF-8.
std::string utf16ToUtf8(const uint16_t* w, size_t count);

// UTF-16LE byte stream -> UTF-8 (byteCount need not be even; partial trailing
// units are ignored). BOM (U+FEFF) is stripped when present.
std::string utf16leToUtf8(const uint8_t* bytes, size_t byteCount);
inline std::string utf16leToUtf8(const std::vector<uint8_t>& bytes)
{
    return utf16leToUtf8(bytes.data(), bytes.size());
}

// UTF-8 -> UTF-16LE bytes (no BOM, no terminator).
std::vector<uint8_t> utf8ToUtf16le(const std::string& s);

// REG_SZ/REG_EXPAND_SZ/REG_MULTI_SZ payload -> UTF-8 list of strings.
std::vector<std::string> regMultiFromBytes(const uint8_t* data, size_t byteCount);

// list of strings -> REG_MULTI_SZ payload (NUL separated, double NUL ended).
std::vector<uint8_t> regMultiToBytes(const std::vector<std::string>& list);

} // namespace bootroll
