#pragma once
#include <windows.h>

namespace desktop_groups {
// Uses the live Explorer desktop view and leaves files and shell settings alone.
bool setDesktopIconsHidden(bool hide);
bool restoreDesktopIcons();
int watchDesktopIcons(DWORD helperPid);
}
