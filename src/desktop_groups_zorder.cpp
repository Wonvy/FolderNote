#include "desktop_groups_zorder.h"
#include <algorithm>

namespace desktop_groups {
bool lowerDesktopWindows(HWND host, std::span<const HWND> windows, HWND preview, bool desktopForeground) {
  if (!host || windows.empty()) return false;
  const bool previewVisible = preview && IsWindowVisible(preview);
  const bool desktopBand = desktopForeground && !previewVisible;
  auto above = GetWindow(host, GW_HWNDPREV);
  while (above && (!IsWindowVisible(above) ||
      std::find(windows.begin(), windows.end(), above) != windows.end())) above = GetWindow(above, GW_HWNDPREV);

  // Show Desktop can raise its host past a preview that does not activate.
  // Choose the lower anchor, so a preview never raises boxes over other apps.
  if (previewVisible && !(GetWindowLongPtrW(preview, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
    bool alreadyBelow = above == preview;
    for (auto candidate = GetWindow(preview, GW_HWNDNEXT); candidate && !alreadyBelow;
         candidate = GetWindow(candidate, GW_HWNDNEXT)) alreadyBelow = candidate == above;
    if (!alreadyBelow) above = preview;
  }
  if (desktopBand) {
    if (!above || !(GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) above = HWND_TOPMOST;
  } else if (!above || (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST)) above = HWND_TOP;

  constexpr UINT flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
  bool success = true;
  for (const auto item : windows) {
    if (IsIconic(item)) ShowWindow(item, SW_SHOWNOACTIVATE);
    const bool topmost = (GetWindowLongPtrW(item, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (topmost != desktopBand)
      success &= SetWindowPos(item, desktopBand ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, flags) != FALSE;
    const bool visible = IsWindowVisible(item) != FALSE;
    auto predecessor = GetWindow(item, GW_HWNDPREV);
    while (predecessor && !IsWindowVisible(predecessor)) predecessor = GetWindow(predecessor, GW_HWNDPREV);
    const bool positioned = above == HWND_TOP || above == HWND_TOPMOST
        ? !predecessor || (!desktopBand && (GetWindowLongPtrW(predecessor, GWL_EXSTYLE) & WS_EX_TOPMOST))
        : predecessor == above;
    if (!visible || !positioned)
      success &= SetWindowPos(item, above, 0, 0, 0, 0, flags | (visible ? 0 : SWP_SHOWWINDOW)) != FALSE;
    above = item;
  }
  return success;
}
}
