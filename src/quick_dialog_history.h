#pragma once
#include <windows.h>
#include <string>

namespace quick::dialog {
std::wstring currentPath(HWND target);
// SEH-guarded wrapper for helper/in-process callers: contains third-party UIA
// access violations and disables further in-process reads after a fault.
std::wstring safeCurrentPath(HWND target) noexcept;
bool pathReaderAvailable() noexcept;
// Called on the helper thread with a validated foreground file dialog, or null.
void observeHistory(HWND target) noexcept;
void stopHistory() noexcept;
}
