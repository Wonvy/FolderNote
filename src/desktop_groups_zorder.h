#pragma once
#include <windows.h>
#include <span>

namespace desktop_groups {
HWND desktopWindowHost();
bool attachDesktopWindow(HWND window, HWND host);
// Match the Shell parent's DPI context at creation, without reparenting a
// differently aware HWND and resetting the whole process's DPI awareness.
struct DesktopDpiScope {
  DPI_AWARENESS_CONTEXT previous = nullptr;
  explicit DesktopDpiScope(HWND host);
  ~DesktopDpiScope();
  DesktopDpiScope(const DesktopDpiScope &) = delete;
};
POINT desktopWindowPoint(HWND window, POINT screen);
bool placeDesktopWindow(HWND window, RECT screen, UINT flags = 0);
bool insideDesktopWindow(HWND window, HWND child);
void focusDesktopWindow(HWND window, HWND control);
// Children supplied front to back; changes only the desktop's sibling order.
bool lowerDesktopWindows(HWND host, std::span<const HWND> windows);
}
