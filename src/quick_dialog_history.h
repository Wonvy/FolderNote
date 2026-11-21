#pragma once
#include <windows.h>
#include <string>

namespace quick::dialog {
std::wstring currentPath(HWND target);
// Called on the helper thread with a validated foreground file dialog, or null.
void observeHistory(HWND target) noexcept;
void stopHistory() noexcept;
}
