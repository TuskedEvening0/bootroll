#include "platform/win/PlatformWin.h"
#include "platform/win/DiskWin.h"
#include "platform/win/ElevateWin.h"
#include "platform/win/VolumeWin.h"
#include "platform/win/WinUtil.h"

#include <algorithm>
#include <commdlg.h>
#include <iterator>

namespace bootroll {

namespace {

// PerMonitorV2 awareness must be active before any DPI query and before the
// first window is created, otherwise Windows bitmap-stretches the whole app.
void enablePerMonitorDpi()
{
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // may fail pre-1703; harmless
    }
}

FirmwareType detectFirmwareType()
{
    using GetFirmwareTypeFn = BOOL(WINAPI*)(FIRMWARE_TYPE*);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (auto fn = reinterpret_cast<GetFirmwareTypeFn>(GetProcAddress(k32, "GetFirmwareType"))) {
        FIRMWARE_TYPE t = FirmwareTypeUnknown;
        if (fn(&t)) {
            if (t == FirmwareTypeBios) {
                return FirmwareType::Bios;
            }
            if (t == FirmwareTypeUefi) {
                return FirmwareType::Uefi;
            }
        }
    }
    return FirmwareType::Unknown;
}

std::string exePathUtf8()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return utf8FromWide(exe);
}

std::string dirOf(const std::string& path)
{
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string() : path.substr(0, p);
}

bool fileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Probe a volume for a file (used to locate the ESP BCD without a drive letter).
std::string probeVolumeForFile(const std::wstring& volumeRoot, const wchar_t* relative)
{
    std::wstring path = volumeRoot + relative;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return {};
    }
    CloseHandle(h);
    return utf8FromWide(path);
}

// Common file-open/save dialog. filter uses '|' separators, converted here to
// the double-NUL pair list GetOpenFileNameW expects.
std::string runFileDialog(bool save, const std::string& title,
                          const std::string& filter, const std::string& defaultExt)
{
    wchar_t buf[MAX_PATH] = {};
    std::wstring wTitle = wideFromUtf8(title);
    std::wstring wFilter = wideFromUtf8(filter);
    for (wchar_t& c : wFilter) {
        if (c == L'|') {
            c = L'\0';
        }
    }
    wFilter.push_back(L'\0'); // filter pair terminator + final string NUL
    std::wstring wExt = wideFromUtf8(defaultExt);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = wFilter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)std::size(buf);
    ofn.lpstrTitle = wTitle.c_str();
    ofn.lpstrDefExt = wExt.empty() ? nullptr : wExt.c_str();
    ofn.Flags = OFN_NOCHANGEDIR | OFN_HIDEREADONLY |
                (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn)) {
        return utf8FromWide(buf);
    }
    return {};
}

} // namespace

std::unique_ptr<IDiskAccess> PlatformWin::createDiskAccess()
{
    return std::make_unique<DiskWin>();
}

std::vector<VolumeInfo> PlatformWin::enumerateVolumes()
{
    return enumerateVolumesWin();
}

bool PlatformWin::readVolumeFirstSector(const std::string& driveLetter,
                                        std::vector<uint8_t>* out, std::string* error)
{
    out->clear();
    if (driveLetter.size() < 2 || driveLetter[1] != ':') {
        *error = "Invalid drive letter: " + driveLetter;
        return false;
    }
    const std::wstring path = L"\\\\.\\" + wideFromUtf8(driveLetter);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = "Cannot open " + driveLetter + ": " + lastErrorMessage(GetLastError());
        return false;
    }
    out->resize(512);
    DWORD got = 0;
    const BOOL ok = ReadFile(h, out->data(), (DWORD)out->size(), &got, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok || got != 512) {
        out->clear();
        *error = "Read failed on " + driveLetter + ": " + lastErrorMessage(err);
        return false;
    }
    return true;
}

std::string PlatformWin::openFileDialog(const std::string& title,
                                        const std::string& filter)
{
    return runFileDialog(false, title, filter, {});
}

std::string PlatformWin::saveFileDialog(const std::string& title,
                                        const std::string& filter,
                                        const std::string& defaultExt)
{
    return runFileDialog(true, title, filter, defaultExt);
}

bool PlatformWin::confirmDialog(const std::string& title, const std::string& text)
{
    // A real OS dialog: it lives above the app window and cannot be trapped
    // inside the ImGui viewport. Blocking the frame here is fine — the same
    // tradeoff the file dialogs already make.
    const int choice = MessageBoxW(GetActiveWindow(), wideFromUtf8(text).c_str(),
                                   wideFromUtf8(title).c_str(),
                                   MB_OKCANCEL | MB_ICONWARNING | MB_SETFOREGROUND);
    return choice == IDOK;
}

FirmwareType PlatformWin::firmwareType()
{
    return detectFirmwareType();
}

float PlatformWin::dpiScale() const
{
    using Fn = UINT(WINAPI*)();
    UINT dpi = 96;
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(user32, "GetDpiForSystem"))) {
            dpi = fn();
        }
    }
    return std::clamp(float(dpi) / 96.0f, 1.0f, 3.0f);
}

std::string PlatformWin::systemBcdPath()
{
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring sysDrive(winDir); // "C:\Windows" -> "C:"
    if (sysDrive.size() >= 2 && sysDrive[1] == L':') {
        sysDrive = sysDrive.substr(0, 2);

        // BIOS: %SystemDrive%\Boot\BCD
        std::wstring biosBcd = sysDrive + L"\\Boot\\BCD";
        if (fileExists(biosBcd)) {
            return utf8FromWide(biosBcd);
        }
    }

    // UEFI: probe volumes for \EFI\Microsoft\Boot\BCD (lettered volumes for now;
    // letter-less ESP probing completes in M1 with the BCD work).
    static const wchar_t* kEspBcd = L"\\EFI\\Microsoft\\Boot\\BCD";
    for (const auto& v : enumerateVolumesWin()) {
        if (!v.driveLetter.empty()) {
            std::string p = probeVolumeForFile(wideFromUtf8(v.driveLetter), kEspBcd);
            if (!p.empty()) {
                return p;
            }
        }
    }
    return {};
}

std::vector<std::string> PlatformWin::candidateFontPaths()
{
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring fonts = std::wstring(winDir) + L"\\Fonts\\";
    const wchar_t* names[] = { L"msyh.ttc", L"msyh.ttf", L"msyhl.ttc", L"simsun.ttc" };
    std::vector<std::string> out;
    for (const wchar_t* n : names) {
        std::wstring p = fonts + n;
        if (fileExists(p)) {
            out.push_back(utf8FromWide(p));
        }
    }
    return out;
}

bool PlatformWin::isElevated()
{
    return isProcessElevated();
}

bool PlatformWin::restartElevated(const std::string& args)
{
    // Qualified call: ElevateWin.h's free function (member name hides it unqualified).
    return bootroll::restartElevated(wideFromUtf8(args).c_str());
}

std::string PlatformWin::exeDir() const
{
    return dirOf(exePathUtf8());
}

std::string PlatformWin::iniPath()
{
    return exeDir() + "\\bootroll.ini";
}

std::string PlatformWin::logPath()
{
    return exeDir() + "\\bootroll.log";
}

std::unique_ptr<IPlatform> createPlatform()
{
    enablePerMonitorDpi();
    return std::make_unique<PlatformWin>();
}

} // namespace bootroll
