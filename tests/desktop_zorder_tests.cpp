#include "../src/desktop_groups_zorder.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool above(HWND first, HWND second) {
  for (auto next = GetWindow(first, GW_HWNDNEXT); next; next = GetWindow(next, GW_HWNDNEXT))
    if (next == second) return true;
  return false;
}
void place(HWND window, HWND after) {
  check(SetWindowPos(window, after, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER), "position real fixture windows");
}
}

int main() {
  std::vector<HWND> owned;
  int result = 0;
  try {
    // Real HWNDs and the real window manager, entirely off screen. Do not
    // activate, minimize, or move the user's applications or desktop.
    const auto make = [&](HWND owner = nullptr, bool visible = true) {
      auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"FolderNote z-order test",
          WS_POPUP | (visible ? WS_VISIBLE : 0), -32000, -32000, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
      check(window != nullptr, "create isolated windows"); owned.push_back(window); return window;
    };
    const auto owner = make(nullptr, false), host = make(), app = make(), preview = make();
    const HWND boxes[]{make(owner), make(owner), make()}; // two owned boxes and an independent shadow
    const auto foreground = GetForegroundWindow();
    const auto normal = [&] {
      for (auto box : boxes) check(!(GetWindowLongPtrW(box, GWL_EXSTYLE) & WS_EX_TOPMOST), "boxes and shadows leave the desktop band for previews and applications");
      check(above(boxes[0], boxes[1]) && above(boxes[1], boxes[2]), "preserve panel and shadow order");
      check(GetForegroundWindow() == foreground, "stacking never changes keyboard focus");
    };

    place(host, HWND_BOTTOM); place(preview, HWND_TOP); place(app, HWND_TOP);
    for (auto box : boxes) place(box, HWND_TOPMOST);
    check(desktop_groups::lowerDesktopWindows(host, boxes, preview), "repair previously pinned boxes");
    normal();
    check(above(app, boxes[0]) && above(preview, boxes[0]) && above(boxes[2], host),
          "boxes remain below apps and preview, above the desktop");

    // Reproduce Show Desktop keeping its host foreground while a preview
    // opens without activation beneath it. Repeated timer ticks must not pin.
    place(host, HWND_TOPMOST);
    for (int tick = 0; tick < 5; ++tick) {
      check(desktop_groups::lowerDesktopWindows(host, boxes, preview, true), "stack with a raised desktop host");
      normal();
      check(above(preview, boxes[0]), "a non-activating preview remains above every box");
    }
    ShowWindow(preview, SW_HIDE);
    check(desktop_groups::lowerDesktopWindows(host, boxes, nullptr, true), "Win+D reveals boxes above the special desktop host");
    for (auto box : boxes) check((GetWindowLongPtrW(box, GWL_EXSTYLE) & WS_EX_TOPMOST) && above(box, host),
                                "boxes remain visible when Show Desktop raises its wallpaper");
    check(GetForegroundWindow() == foreground, "showing desktop boxes preserves focus");
    ShowWindow(preview, SW_SHOWNOACTIVATE);
    check(desktop_groups::lowerDesktopWindows(host, boxes, preview, true), "preview exits the temporary desktop band immediately");
    normal();
    check(above(preview, boxes[0]), "desktop preview covers boxes after Win+D");
    ShowWindow(preview, SW_HIDE);
    check(desktop_groups::lowerDesktopWindows(host, boxes), "handle a topmost host without Seer");
    normal();

    ShowWindow(preview, SW_SHOWNOACTIVATE); place(preview, HWND_TOPMOST);
    check(desktop_groups::lowerDesktopWindows(host, boxes, preview, true), "handle a pinned Seer preview");
    normal();
    check(above(preview, boxes[0]), "pinning Seer does not pin boxes");

    ShowWindow(preview, SW_HIDE); place(host, HWND_NOTOPMOST); place(host, HWND_BOTTOM);
    check(desktop_groups::lowerDesktopWindows(host, boxes), "restore ordinary desktop stacking after closing preview");
    normal();
    check(above(app, boxes[0]) && above(boxes[2], host), "closing preview preserves normal desktop placement");
    std::cout << "PASS real HWND stacking: raised desktop, inactive preview, pinned preview, repeated ticks and close\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
  for (auto i = owned.rbegin(); i != owned.rend(); ++i) DestroyWindow(*i);
  return result;
}
