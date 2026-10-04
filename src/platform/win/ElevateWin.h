#pragma once
// UAC elevation helpers (Windows).
namespace bootroll {

// True when the process token has the elevation attribute.
bool isProcessElevated();

// Restart this process via ShellExecuteExW("runas"). Returns false if declined/failed.
bool restartElevated(const wchar_t* args);

} // namespace bootroll
