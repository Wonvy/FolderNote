#pragma once
#include <windows.h>
#include <span>

namespace desktop_groups {
// Windows are supplied front to back. Show Desktop needs a temporary upper
// band, released as soon as a preview or ordinary application is active.
bool lowerDesktopWindows(HWND host, std::span<const HWND> windows, HWND preview = nullptr, bool desktopForeground = false);
}
