#pragma once
// Win32-layer helpers: UTF-8 <-> UTF-16 conversion + error formatting.
// Never included from core/ or ui/.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <string>

namespace bootroll {

inline std::string utf8FromWide(const std::wstring& w)
{
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                            out.data(), n, nullptr, nullptr);
    }
    return out;
}

inline std::wstring wideFromUtf8(const std::string& s)
{
    if (s.empty()) {
        return {};
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    }
    return out;
}

inline std::string lastErrorMessage(DWORD code)
{
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::string msg = utf8FromWide(std::wstring(buf ? buf : L"", n));
    if (buf) {
        LocalFree(buf);
    }
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) {
        msg.pop_back();
    }
    return msg;
}

inline std::string hexByte(unsigned v)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X", v & 0xFF);
    return buf;
}

} // namespace bootroll
