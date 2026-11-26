#pragma once
#include <windows.h>
#include <string>

// Lightweight, process-safe diagnostics shared by every FolderNote role.
// Everything here is noexcept: diagnostics must never become a failure source.
namespace guard {
// Appends one line to %LOCALAPPDATA%\Programs\FolderNote\Logs\FolderNote.log.
// Rotates at 1 MiB keeping three files; serialized across processes.
void log(const std::wstring &message) noexcept;
// Records unhandled SEH crashes: writes a minidump to ...\FolderNote\Crashes
// (keeping the newest five) and logs module+offset, then lets Windows
// terminate the process normally so the exit code still reports the crash.
void installCrashHandler(const wchar_t *role) noexcept;
// Last `lines` log lines, oldest first, CRLF separated.
std::wstring recentLog(size_t lines) noexcept;
// One-line summary of the newest crash dump ("无" when none).
std::wstring lastCrash() noexcept;
// True for NTSTATUS-style failure exit codes (access violation, fail-fast...).
inline bool crashExitCode(DWORD code) noexcept { return code >= 0xC0000000u && code != STILL_ACTIVE; }
}
