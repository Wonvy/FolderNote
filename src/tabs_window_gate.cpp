#include "tabs_window_gate.h"
#include "tabs_routing.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <atomic>
#include <new>
#include <string>
#include <utility>

namespace tabs::gate {
namespace {
constexpr UINT_PTR gateId = 0x464e5747;
constexpr ULONGLONG lifetime = 20000;
std::atomic<long> objects{0};
struct Gate {
  HWND window;
  HWND origin = nullptr;
  ULONGLONG deadline = GetTickCount64() + lifetime;
  unsigned refs = 1;
  bool attached = false, ownsCloak = false, releasing = false, releaseRequested = false, destroyed = false, originPublished = false;
  Gate(HWND value, HWND priorForeground) : window(value), origin(priorForeground) { ++objects; }
  ~Gate() { --objects; }
  void addRef() { ++refs; }
  void release() { if (!--refs) delete this; }
};
struct Ref {
  Gate *value;
  explicit Ref(Gate *gate, bool add = true) : value(gate) { if (add) value->addRef(); }
  ~Ref() { value->release(); }
};
LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
void detach(Gate *gate) {
  if (!gate->attached) return;
  gate->attached = false;
  auto window = std::exchange(gate->window, nullptr);
  KillTimer(window, gateId);
  if (GetPropW(window, routing::gateProperty) == gate) {
    if (gate->originPublished && GetPropW(window, routing::gateOriginProperty) == gate->origin)
      RemovePropW(window, routing::gateOriginProperty);
    RemovePropW(window, routing::gateProperty);
  }
  RemoveWindowSubclass(window, proc, gateId);
  gate->release(); // The frame's ownership, independent of its browser site.
}
void reveal(Gate *gate) {
  if (!gate->attached || gate->releasing) return;
  gate->releaseRequested = true;
  gate->releasing = true;
  bool restored = true;
  if (gate->ownsCloak) {
    const BOOL cloak = FALSE;
    restored = SUCCEEDED(DwmSetWindowAttribute(gate->window, DWMWA_CLOAK, &cloak, sizeof(cloak)));
    if (!restored && gate->attached) {
      DWORD state = 0;
      restored = SUCCEEDED(DwmGetWindowAttribute(gate->window, DWMWA_CLOAKED, &state, sizeof(state))) &&
                 !(state & DWM_CLOAKED_APP);
    }
  }
  gate->releasing = false;
  // If DWM temporarily rejects restoring presentation, keep ownership and the
  // cleanup timer so a hidden frame cannot be abandoned without a release path.
  if (restored) { gate->ownsCloak = false; detach(gate); }
}
LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
  auto gate = reinterpret_cast<Gate *>(data); Ref keep(gate);
  if (message == WM_NCDESTROY) {
    // A successfully imported source is closing. Never reveal it for one frame
    // while Explorer disconnects its browser site and destroys the window.
    gate->destroyed = true; gate->ownsCloak = false; detach(gate);
    return DefSubclassProc(window, message, w, l);
  }
  if (message == routing::releaseGateMessage()) { reveal(gate); return 0; }
  if (message == WM_TIMER && w == gateId) {
    if (gate->releaseRequested || GetTickCount64() >= gate->deadline || !routing::enabled()) reveal(gate);
    return 0;
  }
  static const UINT shutdownMsg = RegisterWindowMessageW(L"FolderNote.Navigation.Shutdown.v2");
  static const UINT settingsChanged = RegisterWindowMessageW(L"FolderNote.Navigation.SettingsChanged.v2");
  if (message == shutdownMsg || (message == settingsChanged && !routing::enabled())) reveal(gate);
  return DefSubclassProc(window, message, w, l);
}
bool hiddenExplorerFrame(HWND frame) {
  DWORD process = 0;
  if (!frame || GetWindowThreadProcessId(frame, &process) != GetCurrentThreadId() ||
      process != GetCurrentProcessId() || GetAncestor(frame, GA_ROOT) != frame || IsWindowVisible(frame)) return false;
  wchar_t cls[64]{}, windowsDir[MAX_PATH]{};
  if (!GetClassNameW(frame, cls, ARRAYSIZE(cls)) || (wcscmp(cls, L"CabinetWClass") && wcscmp(cls, L"ExploreWClass"))) return false;
  std::wstring path(32768, L'\0');
  auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size() || !GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir))) return false;
  return !_wcsicmp(path.c_str(), (std::wstring(windowsDir) + L"\\explorer.exe").c_str());
}
bool uncloaked(HWND frame) {
  DWORD state = 0;
  return SUCCEEDED(DwmGetWindowAttribute(frame, DWMWA_CLOAKED, &state, sizeof(state))) && state == 0;
}
}
bool arm(HWND frame, const unsigned long long *generation, unsigned long long expected) noexcept {
  try {
    auto current = [&] { return !generation || *generation == expected; };
    if (!current() || !routing::enabled() || !hiddenExplorerFrame(frame) ||
        GetPropW(frame, routing::gateProperty) || GetPropW(frame, routing::gateOriginProperty) || !uncloaked(frame)) return false;
    const auto origin = GetForegroundWindow();
    auto helper = routing::helperWindow(); if (!helper) return false;
    DWORD_PTR approved = 0;
    if (!SendMessageTimeoutW(helper, routing::holdMessage, reinterpret_cast<WPARAM>(frame), 0,
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 150, &approved) || approved != 1) return false;
    if (!current() || !routing::enabled() || !hiddenExplorerFrame(frame) ||
        GetPropW(frame, routing::gateProperty) || GetPropW(frame, routing::gateOriginProperty) || !uncloaked(frame)) return false;
    auto gate = new (std::nothrow) Gate(frame, origin); if (!gate) return false;
    Ref local(gate, false);
    if (!SetWindowSubclass(frame, proc, gateId, reinterpret_cast<DWORD_PTR>(gate))) return false;
    gate->attached = true; gate->addRef();
    if (!SetPropW(frame, routing::gateProperty, gate)) { reveal(gate); return false; }
    if (origin) {
      if (!SetPropW(frame, routing::gateOriginProperty, origin)) { reveal(gate); return false; }
      gate->originPublished = true;
    }
    if (!SetTimer(frame, gateId, 250, nullptr)) {
      reveal(gate); return false;
    }
    if (!gate->attached || !current() || IsWindowVisible(frame)) { reveal(gate); return false; }
    const BOOL cloak = TRUE;
    gate->ownsCloak = true;
    const auto result = DwmSetWindowAttribute(frame, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (!gate->attached) {
      // DWM can pump destruction or a release request while applying a cloak.
      if (!gate->destroyed && IsWindow(frame)) { const BOOL restore = FALSE; DwmSetWindowAttribute(frame, DWMWA_CLOAK, &restore, sizeof(restore)); }
      return false;
    }
    if (FAILED(result) || !current() || !routing::enabled()) { reveal(gate); return false; }
    return true;
  } catch (...) { return false; }
}
long activeObjects() noexcept { return objects.load(); }
}
