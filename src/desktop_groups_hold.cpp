#include "desktop_groups_hold.h"
#include "quick_folders.h"
#include <cwchar>
#include <cstdlib>
#include <new>

namespace desktop_groups {
namespace {
constexpr DWORD holdDuration = 650;

bool modified() {
  return ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU) |
           GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN) | GetAsyncKeyState(VK_LBUTTON) |
           GetAsyncKeyState(VK_MBUTTON) | GetAsyncKeyState(VK_XBUTTON1) | GetAsyncKeyState(VK_XBUTTON2)) & 0x8000) != 0;
}

bool classIs(HWND window, const wchar_t *expected) {
  wchar_t name[32]{};
  return GetClassNameW(window, name, ARRAYSIZE(name)) && !wcscmp(name, expected);
}

HWND desktopView(HWND under, HWND root) {
  for (HWND current = under; current && current != root; current = GetParent(current))
    if (classIs(current, L"SHELLDLL_DefView")) return current;
  if (under == root) {
    HWND view = nullptr;
    EnumChildWindows(root, [](HWND child, LPARAM data) -> BOOL {
      if (!classIs(child, L"SHELLDLL_DefView")) return TRUE;
      *reinterpret_cast<HWND *>(data) = child;
      return FALSE;
    }, reinterpret_cast<LPARAM>(&view));
    return view;
  }
  return nullptr;
}

bool hiddenIconSurface(HWND view, HWND under, HWND root, POINT point) {
  if (!view || (under != view && under != root)) return false;
  const auto icons = FindWindowExW(view, nullptr, L"SysListView32", nullptr);
  const auto viewThread = GetWindowThreadProcessId(view, nullptr);
  RECT bounds{};
  return viewThread && IsChild(root, view) && GetWindowThreadProcessId(icons, nullptr) == viewThread &&
      !(GetWindowLongPtrW(icons, GWL_STYLE) & WS_VISIBLE) &&
      GetWindowRect(view, &bounds) && PtInRect(&bounds, point);
}

bool desktopAcceptsClick(HWND view) {
  const auto thread = GetWindowThreadProcessId(view, nullptr);
  const auto accepts = [](DWORD id) {
    GUITHREADINFO gui{sizeof(gui)};
    return GetGUIThreadInfo(id, &gui) && !gui.hwndCapture &&
        !(gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE));
  };
  return thread && accepts(thread) && accepts(0);
}
}

struct DesktopHold::State {
  HWND receiver = nullptr;
  UINT holdMessage = 0;
  HANDLE thread = nullptr;
  DWORD threadId = 0;
  HANDLE ready = nullptr;
  HHOOK hook = nullptr;
  POINT point{};
  HWND view = nullptr, root = nullptr;
  DWORD pressedAt = 0;
  bool canceled = false;
  static inline thread_local State *current = nullptr;

  bool moved(POINT next) const {
    return std::abs(next.x - point.x) > GetSystemMetrics(SM_CXDRAG) ||
           std::abs(next.y - point.y) > GetSystemMetrics(SM_CYDRAG);
  }

  bool press(const MSLLHOOKSTRUCT &event) {
    if (view) return true;
    if (!holdMessage || modified()) return false;
    const auto under = WindowFromPoint(event.pt);
    const auto desktopRoot = GetAncestor(under, GA_ROOT);
    if (!quick::isDesktop(desktopRoot)) return false;
    const auto target = desktopView(under, desktopRoot);
    if (!hiddenIconSurface(target, under, desktopRoot, event.pt) || !desktopAcceptsClick(target)) return false;
    root = desktopRoot;
    view = target;
    point = event.pt;
    pressedAt = event.time;
    canceled = false;
    return true;
  }

  bool release(const MSLLHOOKSTRUCT &event) {
    if (!view) return false;
    const bool held = event.time - pressedAt >= holdDuration;
    const bool eligible = !canceled && !moved(event.pt) && !modified() &&
        hiddenIconSurface(view, WindowFromPoint(event.pt), root, event.pt) && desktopAcceptsClick(view);
    // Always consume the matching release. Explorer never saw its press,
    // including when the pointer leaves the background or delivery fails.
    view = root = nullptr;
    if (eligible && (!held || !PostMessageW(receiver, holdMessage, static_cast<WPARAM>(event.pt.x),
                                            static_cast<LPARAM>(event.pt.y)))) {
      // Replay the full click so Explorer can open its own background menu.
      // The hook passes injected events through without interpreting them.
      INPUT click[2]{};
      click[0].type = click[1].type = INPUT_MOUSE;
      click[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
      click[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
      SendInput(ARRAYSIZE(click), click, sizeof(INPUT));
    }
    return true;
  }

  static LRESULT CALLBACK mouse(int code, WPARAM message, LPARAM parameter) {
    if (code != HC_ACTION || !current) return CallNextHookEx(nullptr, code, message, parameter);
    auto &self = *current;
    const auto &event = *reinterpret_cast<const MSLLHOOKSTRUCT *>(parameter);
    if (!(event.flags & LLMHF_INJECTED)) {
      if ((message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK) && self.press(event)) return 1;
      if (message == WM_RBUTTONUP && self.release(event)) return 1;
      if (self.view && ((message == WM_MOUSEMOVE && self.moved(event.pt)) ||
                        message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
                        message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)) self.canceled = true;
    }
    return CallNextHookEx(nullptr, code, message, parameter);
  }

  static DWORD WINAPI run(void *argument) {
    auto &self = *static_cast<State *>(argument);
    current = &self;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    self.hook = SetWindowsHookExW(WH_MOUSE_LL, mouse, GetModuleHandleW(nullptr), 0);
    SetEvent(self.ready);
    if (self.hook) {
      while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
      UnhookWindowsHookEx(self.hook);
    }
    return 0;
  }
};

DesktopHold::~DesktopHold() { stop(); }

bool DesktopHold::start(HWND receiver, UINT holdMessage) {
  if (state) return true;
  state = new (std::nothrow) State;
  if (!state) return false;
  state->receiver = receiver;
  state->holdMessage = holdMessage;
  state->ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (state->ready) state->thread = CreateThread(nullptr, 0, State::run, state, 0, &state->threadId);
  if (!state->thread || WaitForSingleObject(state->ready, INFINITE) != WAIT_OBJECT_0 || !state->hook) {
    stop();
    return false;
  }
  return true;
}

void DesktopHold::stop() {
  if (!state) return;
  if (state->thread) {
    PostThreadMessageW(state->threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(state->thread, INFINITE);
    CloseHandle(state->thread);
  }
  if (state->ready) CloseHandle(state->ready);
  delete state;
  state = nullptr;
}
}
