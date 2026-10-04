#include "platform/win/UefiWin.h"
#include "platform/win/WinUtil.h"

#include <vector>

namespace bootroll {

namespace {

// EFI_GLOBAL_VARIABLE: vendor GUID of Boot####/BootOrder/BootNext/Timeout.
// UEFI spec 2.x, Table "Global Variables": 8BE4DF61-93CA-11D2-AA0D-00E098032B8C.
// (Verified against EDK2 UefiMultiPhase.h; any other byte pattern makes every
// lookup fail with ERROR_ENVVAR_NOT_FOUND even on healthy firmware.)
const wchar_t kGlobalGuid[] = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

constexpr DWORD kFirstReadSize = 1024;    // covers every boot variable in practice
constexpr size_t kMaxReadSize = 64 * 1024; // firmware variables are far below this

std::string makeError(const char* action, const std::string& name, DWORD code)
{
    std::string s(action);
    if (!name.empty()) {
        s += " '" + name + "'";
    }
    s += ": " + lastErrorMessage(code) + " (error " + std::to_string(code) + ")";
    return s;
}

} // namespace

bool UefiWin::ensureReady(std::string* error)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        m_lastCode = GetLastError();
        *error = makeError("Cannot open the process token", "", m_lastCode);
        return false;
    }
    TOKEN_PRIVILEGES tp {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME,
                               &tp.Privileges[0].Luid)) {
        const DWORD e = GetLastError();
        CloseHandle(token);
        m_lastCode = e;
        *error = makeError("Cannot look up the system-environment privilege", "", e);
        return false;
    }
    AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    const DWORD e = GetLastError();
    CloseHandle(token);
    if (e == ERROR_NOT_ALL_ASSIGNED) {
        // The token does not carry the privilege: the process must run elevated.
        m_lastCode = e;
        *error = "Administrator privilege is required to read UEFI variables.";
        return false;
    }
    m_lastCode = 0;
    return true;
}

bool UefiWin::read(const std::string& name, std::vector<uint8_t>* data,
                   uint32_t* attrs, std::string* error)
{
    const std::wstring wname = wideFromUtf8(name);
    std::vector<uint8_t> buf(kFirstReadSize);
    DWORD len = 0;
    DWORD varAttrs = 0;
    for (;;) {
        len = GetFirmwareEnvironmentVariableExW(wname.c_str(), kGlobalGuid,
                                                buf.data(), DWORD(buf.size()),
                                                &varAttrs);
        if (len != 0) {
            break;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_INSUFFICIENT_BUFFER && buf.size() < kMaxReadSize) {
            buf.resize(buf.size() * 8);
            continue;
        }
        if (e == ERROR_SUCCESS) {
            len = 0; // variable exists but is empty
            break;
        }
        m_lastCode = e;
        *error = makeError("Cannot read UEFI variable", name, e);
        return false;
    }
    m_lastCode = 0;
    data->assign(buf.data(), buf.data() + len);
    *attrs = varAttrs;
    return true;
}

bool UefiWin::write(const std::string& name, const std::vector<uint8_t>& data,
                    uint32_t attrs, std::string* error)
{
    const std::wstring wname = wideFromUtf8(name);
    const BOOL ok = SetFirmwareEnvironmentVariableExW(
        wname.c_str(), kGlobalGuid,
        data.empty() ? nullptr : reinterpret_cast<PVOID>(const_cast<uint8_t*>(data.data())),
        DWORD(data.size()), attrs);
    if (!ok) {
        m_lastCode = GetLastError();
        *error = makeError("Cannot write UEFI variable", name, m_lastCode);
        return false;
    }
    m_lastCode = 0;
    return true;
}

bool UefiWin::remove(const std::string& name, std::string* error)
{
    const std::wstring wname = wideFromUtf8(name);
    // A null payload with size 0 deletes the variable.
    const BOOL ok = SetFirmwareEnvironmentVariableExW(wname.c_str(), kGlobalGuid,
                                                      nullptr, 0, 0);
    if (!ok) {
        m_lastCode = GetLastError();
        *error = makeError("Cannot delete UEFI variable", name, m_lastCode);
        return false;
    }
    m_lastCode = 0;
    return true;
}

} // namespace bootroll
