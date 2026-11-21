#include "desktop_groups_zorder.h"

namespace desktop_groups {
HWND desktopWindowHost() {
  struct Search { DWORD process = 0; HWND view = nullptr; } search;
  GetWindowThreadProcessId(GetShellWindow(), &search.process);
  if (!search.process) return nullptr;
  EnumWindows([](HWND root, LPARAM data) -> BOOL {
    auto &search = *reinterpret_cast<Search *>(data);
    DWORD process = 0; GetWindowThreadProcessId(root, &process);
    wchar_t name[64]{}; GetClassNameW(root, name, ARRAYSIZE(name));
    if (process != search.process || (wcscmp(name, L"Progman") && wcscmp(name, L"WorkerW"))) return TRUE;
    EnumChildWindows(root, [](HWND child, LPARAM data) -> BOOL {
      auto &search = *reinterpret_cast<Search *>(data);
      wchar_t name[64]{}; GetClassNameW(child, name, ARRAYSIZE(name));
      DWORD process = 0; GetWindowThreadProcessId(child, &process);
      if (process == search.process && !wcscmp(name, L"SHELLDLL_DefView")) {
        search.view = child; return FALSE;
      }
      return TRUE;
    }, data);
    return !search.view;
  }, reinterpret_cast<LPARAM>(&search));
  return search.view;
}
DesktopDpiScope::DesktopDpiScope(HWND host) {
  if (const auto context = GetWindowDpiAwarenessContext(host)) previous = SetThreadDpiAwarenessContext(context);
}
DesktopDpiScope::~DesktopDpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
bool attachDesktopWindow(HWND window, HWND host) {
  // Initialize WinUI under an in-process root first. Attach while hidden and
  // with matching DPI contexts, before any pixels can enter the app Z-order.
  if (!IsWindow(window) || !IsWindow(host) || IsWindowVisible(window) ||
      !AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(window), GetWindowDpiAwarenessContext(host))) return false;
  RECT bounds{};
  if (!GetWindowRect(window, &bounds)) return false;
  const auto style = GetWindowLongPtrW(window, GWL_STYLE);
  SetLastError(ERROR_SUCCESS);
  SetWindowLongPtrW(window, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD);
  if (GetLastError()) return false;
  SetLastError(ERROR_SUCCESS);
  if (!SetParent(window, host) && GetLastError()) {
    SetWindowLongPtrW(window, GWL_STYLE, style);
    return false;
  }
  return placeDesktopWindow(window, bounds, SWP_FRAMECHANGED);
}
POINT desktopWindowPoint(HWND window, POINT screen) {
  if (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) ScreenToClient(GetParent(window), &screen);
  return screen;
}
bool placeDesktopWindow(HWND window, RECT screen, UINT flags) {
  const auto point = desktopWindowPoint(window, {screen.left, screen.top});
  return SetWindowPos(window, nullptr, point.x, point.y, screen.right - screen.left, screen.bottom - screen.top,
      flags | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}
bool insideDesktopWindow(HWND window, HWND child) {
  return window && child && (window == child || IsChild(window, child));
}
void focusDesktopWindow(HWND window, HWND control) {
  if (!insideDesktopWindow(window, control)) return;
  SetForegroundWindow(GetAncestor(window, GA_ROOT));
  SetFocus(control);
}
bool lowerDesktopWindows(HWND host, std::span<const HWND> windows) {
  if (!host) return false;
  HWND previous = HWND_TOP;
  bool success = true;
  for (auto window : windows) {
    // Reorder siblings inside the desktop view, never the global topmost band.
    if (!IsWindow(window) || GetParent(window) != host || !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)) {
      success = false; continue;
    }
    const auto predecessor = GetWindow(window, GW_HWNDPREV);
    const bool positioned = previous == HWND_TOP ? !predecessor : predecessor == previous;
    const bool visible = (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
    if (!visible || !positioned)
      success &= SetWindowPos(window, previous, 0, 0, 0, 0,
          SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (visible ? 0 : SWP_SHOWWINDOW)) != FALSE;
    previous = window;
  }
  return success;
}
}
