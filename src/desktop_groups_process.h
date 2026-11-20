#pragma once

#include <windows.h>
#include <string>

namespace desktop_groups {
enum class DesktopLaunchMode { Direct, Explorer, Unavailable };
enum class DesktopReceiverStatus { Compatible, Elevated, Unavailable };

struct DesktopLaunchContext {
  DesktopLaunchMode mode = DesktopLaunchMode::Unavailable;
  DWORD shellPid = 0;
  DWORD shellIntegrity = 0;
};

DesktopLaunchContext desktopLaunchContext();
DesktopReceiverStatus desktopReceiverStatus(HWND window, const DesktopLaunchContext &context);
bool executeFromDesktop(const std::wstring &exe, const wchar_t *arguments, const DesktopLaunchContext &context);
}
