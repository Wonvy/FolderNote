#pragma once
#include <windows.h>
#include <string>

namespace desktop_groups {
bool desktopPreviewAvailable();
// The IPC endpoint is hidden. Find the actual visible Seer window separately.
HWND desktopPreviewWindow();
// Sends the selected filesystem item to a running Seer. Returns whether the
// preview key was consumed; Seer itself toggles a repeated path closed.
bool previewDesktopItem(const std::wstring &path);
}
