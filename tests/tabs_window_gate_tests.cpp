// All frames stay natively hidden. DWM, helper approval and display events are
// simulated; no user window is cloaked and no running helper is contacted.
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include "../src/navigation.h"
#include <functional>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
struct WindowState {
  DWORD cloak = 0;
  bool visible = false;
  unsigned cloaks = 0, releases = 0;
  std::vector<std::string> events;
};
std::unordered_map<HWND, WindowState> windows;
HWND fakeHelper = nullptr;
HWND foreground = nullptr;
bool approve = true, failRead = false, failCloak = false, failRelease = false, failTimer = false, failSend = false;
unsigned approvals = 0;
ULONGLONG now = 1000;
std::function<void(HWND)> onApproval, onCloak;
DWORD testProcessPath(HMODULE module, LPWSTR text, DWORD capacity) {
  if (module) return GetModuleFileNameW(module, text, capacity);
  wchar_t system[MAX_PATH]{}; GetWindowsDirectoryW(system, ARRAYSIZE(system));
  auto path = std::wstring(system) + L"\\explorer.exe";
  if (path.size() >= capacity) return capacity;
  memcpy(text, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); return DWORD(path.size());
}
BOOL testVisible(HWND window) { auto found = windows.find(window); return found == windows.end() ? IsWindowVisible(window) : found->second.visible; }
HWND testFindWindow(HWND parent, HWND after, LPCWSTR cls, LPCWSTR title) {
  if (parent == HWND_MESSAGE && reinterpret_cast<ULONG_PTR>(cls) > 0xffff && !wcscmp(cls, L"FolderNote.QuickFolders.Helper.v1")) return fakeHelper;
  return FindWindowExW(parent, after, cls, title);
}
LRESULT testSend(HWND destination, UINT message, WPARAM w, LPARAM l, UINT flags, UINT timeout, PDWORD_PTR result) {
  if (destination != fakeHelper || message != WM_APP + 130 || l || timeout != 150 || !(flags & SMTO_ABORTIFHUNG)) return 0;
  ++approvals; auto frame = reinterpret_cast<HWND>(w); windows[frame].events.push_back("hold");
  if (onApproval) { auto call = std::move(onApproval); call(frame); }
  if (failSend) return 0;
  if (result) *result = approve ? 1 : 0; return 1;
}
HRESULT testGetDwm(HWND frame, DWORD attribute, PVOID value, DWORD bytes) {
  if (failRead || attribute != DWMWA_CLOAKED || bytes != sizeof(DWORD) || !windows.contains(frame)) return E_FAIL;
  *static_cast<DWORD *>(value) = windows[frame].cloak; return S_OK;
}
HRESULT testSetDwm(HWND frame, DWORD attribute, LPCVOID value, DWORD bytes) {
  if (attribute != DWMWA_CLOAK || bytes != sizeof(BOOL) || !windows.contains(frame)) return E_FAIL;
  auto &state = windows[frame]; const bool cloak = *static_cast<const BOOL *>(value) != FALSE;
  state.events.push_back(cloak ? "cloak" : "release");
  if (cloak) ++state.cloaks; else ++state.releases;
  if ((cloak && failCloak) || (!cloak && failRelease)) return E_FAIL;
  if (cloak) state.cloak |= DWM_CLOAKED_APP; else state.cloak &= ~DWM_CLOAKED_APP;
  if (cloak && onCloak) { auto call = std::move(onCloak); call(frame); }
  return S_OK;
}
ULONGLONG testNow() { return now; }
HWND testForeground() { return foreground; }
UINT_PTR testTimer(HWND window, UINT_PTR id, UINT delay, TIMERPROC procedure) {
  return failTimer ? 0 : SetTimer(window, id, delay, procedure);
}
}
#define GetModuleFileNameW testProcessPath
#define IsWindowVisible testVisible
#define FindWindowExW testFindWindow
#define SendMessageTimeoutW testSend
#define DwmGetWindowAttribute testGetDwm
#define DwmSetWindowAttribute testSetDwm
#define GetTickCount64 testNow
#define GetForegroundWindow testForeground
#define SetTimer testTimer
#include "../src/tabs_window_gate.cpp"
#undef SetTimer
#undef GetTickCount64
#undef GetForegroundWindow
#undef DwmSetWindowAttribute
#undef DwmGetWindowAttribute
#undef SendMessageTimeoutW
#undef FindWindowExW
#undef IsWindowVisible
#undef GetModuleFileNameW

namespace {
int checks = 0;
void require(bool value, const char *message) { ++checks; if (!value) throw message; }
void preference(bool value) {
  DWORD setting = value ? 1 : 0;
  require(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerOpenInNewTab", REG_DWORD, &setting, sizeof(setting)) == ERROR_SUCCESS,
          "save isolated gate preference");
}
struct Frame {
  HWND window = CreateWindowW(L"CabinetWClass", L"Hidden presentation gate fixture", WS_OVERLAPPED,
      0, 0, 600, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Frame() { require(window != nullptr, "create hidden gate frame"); windows.emplace(window, WindowState{}); }
  ~Frame() { if (IsWindow(window)) DestroyWindow(window); windows.erase(window); }
  WindowState &state() { return windows[window]; }
  bool arm() { return tabs::gate::arm(window); }
  void release() { SendMessageW(window, tabs::routing::releaseGateMessage, 0, 0); }
  void timer() { SendMessageW(window, WM_TIMER, tabs::gate::gateId, 0); }
};
struct Registry {
  HKEY real = nullptr, root = nullptr;
  std::wstring path;
  Registry() {
    path = L"Software\\FolderNoteGateTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    require(RegOpenCurrentUser(KEY_ALL_ACCESS, &real) == ERROR_SUCCESS &&
            RegCreateKeyExW(real, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS &&
            RegOverridePredefKey(HKEY_CURRENT_USER, root) == ERROR_SUCCESS, "isolate gate settings");
    DWORD enabled = 1;
    require(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled)) == ERROR_SUCCESS,
            "enable isolated Win10 tabs");
    preference(true);
  }
  ~Registry() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (root) RegCloseKey(root);
    if (real) { RegDeleteTreeW(real, path.c_str()); RegCloseKey(real); }
  }
};
void scenarios() {
  if (!navigation::tabsSupported()) return;
  {
    Frame frame; auto style = GetWindowLongPtrW(frame.window, GWL_STYLE);
    require(frame.arm() && frame.state().cloak == DWM_CLOAKED_APP && tabs::gate::activeObjects() == 1,
            "approved hidden frame is app-cloaked and pins module");
    require(GetPropW(frame.window, tabs::routing::gateProperty) != nullptr &&
            GetPropW(frame.window, tabs::routing::gateOriginProperty) == foreground && !frame.arm(), "gate publication records original foreground and is unique per frame");
    frame.state().visible = true; frame.state().events.push_back("show"); SendMessageW(frame.window, WM_SHOWWINDOW, TRUE, 0);
    require(frame.state().events == std::vector<std::string>{"hold", "cloak", "show"}, "cloak is committed before simulated first show");
    require(GetWindowLongPtrW(frame.window, GWL_STYLE) == style, "gate does not change visibility or layout style bits");
    frame.release();
    require(frame.state().cloak == 0 && !GetPropW(frame.window, tabs::routing::gateProperty) &&
            !GetPropW(frame.window, tabs::routing::gateOriginProperty) && tabs::gate::activeObjects() == 0,
            "release message removes owned cloak and window gate");
  }
  {
    Frame frame; frame.state().visible = true; auto before = approvals;
    require(!frame.arm() && approvals == before && frame.state().cloaks == 0, "already-visible baseline frame never reaches approval or cloak");
  }
  {
    Frame frame; approve = false;
    require(!frame.arm() && frame.state().cloaks == 0 && !GetPropW(frame.window, tabs::routing::gateProperty), "helper denying first window leaves presentation untouched");
    approve = true;
  }
  for (DWORD existing : {DWORD(DWM_CLOAKED_APP), DWORD(DWM_CLOAKED_SHELL), DWORD(DWM_CLOAKED_INHERITED)}) {
    Frame frame; frame.state().cloak = existing;
    require(!frame.arm() && !frame.state().cloaks && !frame.state().releases && frame.state().cloak == existing,
            "preexisting app, desktop-shell and inherited cloak are never adopted or cleared");
  }
  {
    Frame frame; require(frame.arm(), "arm timed gate");
    now += 19999; frame.timer(); require(frame.state().cloak == DWM_CLOAKED_APP, "gate retains presentation until exact deadline");
    ++now; frame.timer(); require(frame.state().cloak == 0 && tabs::gate::activeObjects() == 0, "independent deadline restores failed handoff window");
  }
  {
    Frame frame; require(frame.arm(), "arm settings gate"); preference(false);
    SendMessageW(frame.window, RegisterWindowMessageW(L"FolderNote.Navigation.SettingsChanged.v2"), 0, 0);
    require(frame.state().cloak == 0 && tabs::gate::activeObjects() == 0, "disabled preference immediately releases presentation"); preference(true);
  }
  {
    Frame frame; require(frame.arm(), "arm polling settings gate"); preference(false); frame.timer();
    require(frame.state().cloak == 0, "cleanup timer observes settings changes without a notification"); preference(true);
  }
  {
    Frame frame; require(frame.arm(), "arm shutdown gate");
    SendMessageW(frame.window, RegisterWindowMessageW(L"FolderNote.Navigation.Shutdown.v2"), 0, 0);
    require(frame.state().cloak == 0, "shutdown restores held frame");
  }
  for (int failure = 0; failure < 4; ++failure) {
    Frame frame; failRead = failure == 0; failSend = failure == 1; failTimer = failure == 2; failCloak = failure == 3;
    require(!frame.arm() && !(frame.state().cloak & DWM_CLOAKED_APP) && !GetPropW(frame.window, tabs::routing::gateProperty) && tabs::gate::activeObjects() == 0,
            "DWM query, helper timeout, timer or cloak failure cannot strand a held frame");
    failRead = failSend = failTimer = failCloak = false;
  }
  {
    Frame frame; require(frame.arm(), "arm restore-retry gate"); failRelease = true; frame.release();
    require(tabs::gate::activeObjects() == 1 && GetPropW(frame.window, tabs::routing::gateProperty), "failed uncloak retains cleanup ownership");
    failRelease = false; frame.timer(); require(frame.state().cloak == 0 && tabs::gate::activeObjects() == 0, "temporary uncloak failure retries through cleanup timer");
  }
  {
    Frame frame; require(frame.arm(), "arm owned-cloak gate"); frame.state().cloak |= DWM_CLOAKED_SHELL; frame.release();
    require(frame.state().cloak == DWM_CLOAKED_SHELL, "releasing owned APP cloak preserves later Shell desktop cloak");
  }
  {
    Frame frame; unsigned long long generation = 1;
    onApproval = [&](HWND) { ++generation; };
    require(!tabs::gate::arm(frame.window, &generation, 1) && !frame.state().cloaks,
            "reentrant browser-site replacement during approval aborts before cloaking");
  }
  {
    Frame frame; onApproval = [&](HWND) { frame.state().visible = true; };
    require(!frame.arm() && !frame.state().cloaks, "frame shown while approval is pending is left untouched");
  }
  {
    Frame frame; bool armed = true;
    std::thread other([&] { armed = frame.arm(); }); other.join();
    require(!armed && !frame.state().cloaks, "other thread cannot arm a frame's presentation gate");
  }
  {
    Frame frame; require(frame.arm(), "arm closing source frame"); DestroyWindow(frame.window);
    require(frame.state().releases == 0 && tabs::gate::activeObjects() == 0, "source destruction releases gate without uncloak flash");
  }
  {
    Frame frame; onCloak = [](HWND window) { DestroyWindow(window); };
    require(!frame.arm() && frame.state().releases == 0 && tabs::gate::activeObjects() == 0, "reentrant destruction during cloak is lifetime safe and never reveals source");
  }
  require(tabs::gate::activeObjects() == 0, "all gates release module ownership");
}
void nativeSmoke() {
  // These calls are below all macro undefinitions and use the real compositor.
  // Only this disposable, nonactivating tool window is ever shown, and only
  // after native DWM accepted its application cloak. Destruction never reveals it.
  BOOL composition = FALSE;
  const auto compositionResult = ::DwmIsCompositionEnabled(&composition);
  require(SUCCEEDED(compositionResult), "query native DWM composition availability");
  if (!composition) { std::cout << "SKIP native DWM smoke: composition is disabled\n"; return; }
  struct NativeFrame {
    HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"FolderNote native cloak smoke",
        WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~NativeFrame() { if (::IsWindow(window)) ::DestroyWindow(window); }
  } frame;
  require(frame.window && !::IsWindowVisible(frame.window), "native smoke begins with an unshown tool window");
  DWORD before = MAXDWORD, afterSet = MAXDWORD, afterShow = MAXDWORD;
  const auto readBefore = ::DwmGetWindowAttribute(frame.window, DWMWA_CLOAKED, &before, sizeof(before));
  const BOOL cloak = TRUE;
  const auto applied = ::DwmSetWindowAttribute(frame.window, DWMWA_CLOAK, &cloak, sizeof(cloak));
  const auto readAfterSet = ::DwmGetWindowAttribute(frame.window, DWMWA_CLOAKED, &afterSet, sizeof(afterSet));
  std::cout << "Native DWM before-show: read=0x" << std::hex << static_cast<unsigned long>(readBefore)
            << " state=0x" << before << " set=0x" << static_cast<unsigned long>(applied)
            << " read-after=0x" << static_cast<unsigned long>(readAfterSet) << " state-after=0x" << afterSet << std::dec << "\n";
  require(SUCCEEDED(applied), "native DWM accepts APP cloak before the first ShowWindow");
  ::ShowWindow(frame.window, SW_SHOWNOACTIVATE);
  const auto flushed = ::DwmFlush();
  const auto readAfterShow = ::DwmGetWindowAttribute(frame.window, DWMWA_CLOAKED, &afterShow, sizeof(afterShow));
  std::cout << "Native DWM after-show: flush=0x" << std::hex << static_cast<unsigned long>(flushed)
            << " read=0x" << static_cast<unsigned long>(readAfterShow) << " state=0x" << afterShow << std::dec << "\n";
  require(SUCCEEDED(flushed) && SUCCEEDED(readAfterShow) && (afterShow & DWM_CLOAKED_APP),
          "native APP cloak survives first show and compositor flush without revealing the tool window");
}
}
int main() {
  int result = 0;
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = DefWindowProcW; cls.lpszClassName = L"CabinetWClass";
  if (!RegisterClassW(&cls)) return 2;
  fakeHelper = CreateWindowW(L"STATIC", L"Inert test helper", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
  foreground = fakeHelper;
  try { Registry registry; scenarios(); nativeSmoke(); std::cout << "PASS " << checks << " pre-display gate assertions\n"; }
  catch (const char *message) { std::cerr << "FAIL " << checks << ": " << message << "\n"; result = 1; }
  catch (...) { std::cerr << "FAIL unexpected exception\n"; result = 1; }
  if (fakeHelper) DestroyWindow(fakeHelper); fakeHelper = nullptr;
  UnregisterClassW(cls.lpszClassName, cls.hInstance); return result;
}
