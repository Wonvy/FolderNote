#pragma once
#include <windows.h>

namespace desktop_groups {
// Restores the Explorer view only when it was hidden by an older version of ShibaKit.
bool restoreDesktopIcons();
int watchDesktopIcons(DWORD helperPid);
}
