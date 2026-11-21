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
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER), "position fixture windows");
}
}
int main() {
  using namespace desktop_groups;
  std::vector<HWND> owned;
  int result = 0;
  try {
    const auto make = [&](HWND parent = nullptr) {
      auto window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"FolderNote desktop-layer test",
          (parent ? WS_CHILD : WS_POPUP) | WS_VISIBLE | WS_CLIPSIBLINGS,
          parent ? 0 : -32000, parent ? 0 : -32000, 800, 600, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
      check(window != nullptr, "create off-screen fixture"); owned.push_back(window); return window;
    };
    const auto desktop = make(), host = make(desktop), icons = make(host), app = make(), preview = make();
    const HWND boxes[]{make(host), make(host), make(host)};
    const auto control = make(boxes[0]);
    const auto foreground = GetForegroundWindow();
    const auto normal = [&] {
      for (auto box : boxes) {
        check(!(GetWindowLongPtrW(box, GWL_EXSTYLE) & WS_EX_TOPMOST), "box never enters global topmost band");
        check(GetAncestor(box, GA_ROOT) == desktop && GetParent(box) == host, "box belongs to desktop hierarchy");
        check(IsWindowVisible(box) && !IsIconic(box), "desktop children remain visible and restored");
      }
      check(GetForegroundWindow() == foreground, "layout preserves keyboard focus");
    };
    place(preview, HWND_TOP); place(app, HWND_TOP); place(desktop, preview);
    check(lowerDesktopWindows(host, boxes), "place boxes over native desktop icons");
    normal();
    check(above(boxes[0], boxes[1]) && above(boxes[1], boxes[2]) && above(boxes[2], icons), "preserve panel/shadow order");
    check(insideDesktopWindow(boxes[0], control) && insideDesktopWindow(boxes[0], boxes[0]) &&
          !insideDesktopWindow(boxes[0], icons) && !insideDesktopWindow(boxes[0], app), "nested input stays in its box");
    RECT expected{-31920,-31910,-31720,-31760}, actual{};
    check(placeDesktopWindow(boxes[0], expected) && GetWindowRect(boxes[0], &actual) && EqualRect(&expected,&actual),
        "screen coordinates survive nonzero desktop origin");
    const auto pending = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, expected.left, expected.top, 200, 150, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    owned.push_back(pending);
    check(attachDesktopWindow(pending, host) && GetParent(pending) == host && !IsWindowVisible(pending) &&
          GetWindowRect(pending, &actual) && EqualRect(&expected, &actual), "attach initialized hidden window without a visible top-level frame");
    check(!attachDesktopWindow(app, host) && GetParent(app) == nullptr, "never reparent a visible application");
    const auto shadow = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    owned.push_back(shadow);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), 20, -20, 1, 32, BI_RGB};
    void *pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto dc = CreateCompatibleDC(nullptr);
    const auto old = SelectObject(dc, bitmap);
    POINT origin{expected.left - 10, expected.top - 10}, source{};
    SIZE size{20, 20}; BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    auto position = desktopWindowPoint(shadow, origin);
    const auto layered = UpdateLayeredWindow(shadow, nullptr, &position, &size, dc, &source, 0, &blend, ULW_ALPHA);
    const auto layeredError = GetLastError();
    SelectObject(dc, old); DeleteDC(dc); DeleteObject(bitmap);
    GetWindowRect(shadow, &actual);
    if (!layered || actual.left != origin.x || actual.top != origin.y)
      std::cerr << "layered=" << layered << " error=" << layeredError << " actual=" << actual.left << ',' << actual.top
                << " expected=" << origin.x << ',' << origin.y << '\n';
    check(layered && GetWindowRect(shadow, &actual) && actual.left == origin.x && actual.top == origin.y,
        "layered desktop shadow converts screen to parent coordinates at nonzero desktop origin");
    {
      const auto before = GetThreadDpiAwarenessContext();
      { DesktopDpiScope dpi(host); check(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),GetWindowDpiAwarenessContext(host)), "match host DPI at creation"); }
      check(AreDpiAwarenessContextsEqual(before,GetThreadDpiAwarenessContext()), "restore caller DPI context");
    }
    for (int transition = 0; transition < 25; ++transition) {
      // Simulate Show Desktop lifting its root, then returning to an app.
      // No repair callback or timer runs between these operations/assertions.
      place(desktop, HWND_TOPMOST); normal();
      place(desktop, HWND_NOTOPMOST); place(desktop, preview);
      normal();
      check(above(app, GetAncestor(boxes[0], GA_ROOT)) && above(preview, GetAncestor(boxes[0], GA_ROOT)),
          "app and inactive preview cover boxes immediately without a cleanup tick");
    }
    ShowWindow(desktop,SW_HIDE);
    check(!IsWindowVisible(boxes[0]), "children follow hidden desktop");
    ShowWindow(desktop,SW_SHOWNOACTIVATE); normal();
    const HWND invalid[]{app};
    const auto before = GetWindow(app,GW_HWNDPREV);
    check(!lowerDesktopWindows(host,invalid) && GetWindow(app,GW_HWNDPREV)==before, "reject unrelated top-level windows");
    DestroyWindow(host);
    check(!IsWindow(boxes[0]), "desktop destruction also removes its child boxes");
    const auto replacement = make(desktop), restored = make(replacement);
    const HWND recovered[]{restored};
    check(lowerDesktopWindows(replacement,recovered) && IsWindowVisible(restored), "new desktop view accepts restored boxes");
    std::cout << "PASS desktop children: no transient topmost, app/preview occlusion, show/hide, coordinates, input ancestry, DPI, recovery\n";
  } catch(const std::exception &error) { std::cerr << error.what() << '\n'; result=1; }
  for(auto item=owned.rbegin();item!=owned.rend();++item) if(IsWindow(*item)) DestroyWindow(*item);
  return result;
}
