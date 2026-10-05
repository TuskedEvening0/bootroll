#include "app/CrashHandler.h"

#ifdef _WIN32

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace bootroll {

namespace {

std::wstring exeDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    const size_t p = dir.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring(L".") : dir.substr(0, p);
}

void appendLog(const char* text)
{
    const std::wstring dir = exeDir();
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\crash.log").c_str(), L"ab") != 0 || !f) {
        return;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

void writeMinidump(EXCEPTION_POINTERS* ep)
{
    const std::wstring dir = exeDir();
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\crash_%lu.dmp", dir.c_str(), GetProcessId(GetCurrentProcess()));
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal,
                      &mei, nullptr, nullptr);
    CloseHandle(f);
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep)
{
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
                  "---- crash: exception 0x%08lX at address 0x%p ----\n",
                  ep->ExceptionRecord->ExceptionCode,
                  ep->ExceptionRecord->ExceptionAddress);
    appendLog(buf);

    HANDLE proc = GetCurrentProcess();
    SymInitialize(proc, nullptr, TRUE);

    CONTEXT* ctx = ep->ContextRecord;
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = ctx->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    HANDLE thread = GetCurrentThread();
    char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)] = {};
    for (int i = 0; i < 32; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) {
            continue;
        }
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;
        DWORD64 disp64 = 0;
        const BOOL hasSym = SymFromAddr(proc, pc, &disp64, sym);
        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD disp32 = 0;
        const BOOL hasLine = SymGetLineFromAddr64(proc, pc, &disp32, &line);

        std::snprintf(buf, sizeof(buf), "  #%02d %s+0x%llx  [%s:%lu]\n", i,
                      hasSym ? sym->Name : "<unknown>",
                      hasSym ? (unsigned long long)disp64 : 0ull,
                      hasLine ? line.FileName : "<no source>",
                      hasLine ? (unsigned long)line.LineNumber : 0ul);
        appendLog(buf);
    }
    writeMinidump(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void installCrashHandler()
{
    SetUnhandledExceptionFilter(onCrash);
}

} // namespace bootroll

#else // POSIX (Linux): signal + backtrace, minimal but async-crash usable.

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include <execinfo.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

namespace bootroll {

namespace {

// Exe directory from /proc/self/exe (single-file portable layout).
void crashLogPath(char* buf, size_t size)
{
    char exe[4096] = ".";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        n = 1;
        exe[0] = '.';
    }
    exe[n] = '\0';
    char* slash = strrchr(exe, '/');
    if (slash != nullptr) {
        *slash = '\0';
    }
    std::snprintf(buf, size, "%s/crash.log", exe);
}

void appendText(int fd, const char* text)
{
    if (write(fd, text, strlen(text)) < 0) {
        // Nothing left to do inside a crash handler.
    }
}

// Best-effort crash report. Only async-signal-safe calls (open/write/backtrace)
// plus snprintf - deliberately simpler than the Win32 minidump path.
void onFatalSignal(int sig)
{
    char path[4200];
    crashLogPath(path, sizeof(path));
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "---- crash: signal %d ----\n", sig);
    if (fd >= 0) {
        appendText(fd, buf);
    }
    appendText(STDERR_FILENO, buf);

    void* frames[64];
    const int count = backtrace(frames, 64);
    if (fd >= 0) {
        backtrace_symbols_fd(frames, count, fd);
        close(fd);
    }
    backtrace_symbols_fd(frames, count, STDERR_FILENO);

    // Restore the default disposition and re-raise so the exit status/core
    // still reflect the fatal signal.
    signal(sig, SIG_DFL);
    raise(sig);
}

} // namespace

void installCrashHandler()
{
    for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) {
        signal(sig, onFatalSignal);
    }
}

} // namespace bootroll

#endif
