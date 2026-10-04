#pragma once
// Crash triage: on an unhandled exception, append a symbolized stack trace to
// crash.log next to the executable and write a minidump beside it. Debug-only
// diagnostics aid; harmless in release.
namespace bootroll {

void installCrashHandler();

} // namespace bootroll
