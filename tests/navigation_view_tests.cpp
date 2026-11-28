#include "../src/navigation_view.h"
#include "../src/quick_folders.h"
#include <commctrl.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
bool isolatedMiddle = false;
HWND middleForeground = nullptr, middleView = nullptr;
HWND wheelVisibleBand = nullptr;
bool wheelBandVisible = false;
int middleModifier = 0, menuLaunches = 0;
HWND middleForegroundWindow() { return isolatedMiddle ? middleForeground : GetForegroundWindow(); }
BOOL middleVisible(HWND window) {
  if (window && window == wheelVisibleBand) return wheelBandVisible;
  return isolatedMiddle && window == middleView ? TRUE : IsWindowVisible(window);
}
SHORT middleKeyState(int key) { return isolatedMiddle ? (key == middleModifier ? SHORT(-32768) : 0) : GetAsyncKeyState(key); }
}
namespace quick {
void recordTestMenu(HWND target, HWND view, POINT) {
  if (isolatedMiddle && target == middleForeground && view == middleView) ++menuLaunches;
}
}
// Include the same controller with only foreground/input queries and menu
// launch replaced. All test windows remain hidden and no real input is sent.
#define GetForegroundWindow middleForegroundWindow
#define IsWindowVisible middleVisible
#define GetAsyncKeyState middleKeyState
#define launchMenu recordTestMenu
#include "../src/navigation_view.cpp"
#undef launchMenu
#undef GetAsyncKeyState
#undef IsWindowVisible
#undef GetForegroundWindow

namespace {
int checks = 0;
void check(bool value, const char *reason) {
  ++checks;
  if (!value)
    throw std::runtime_error(reason);
}
class IsolatedSettings {
  HKEY realUser = nullptr, temporary = nullptr;
  std::wstring path;
public:
  IsolatedSettings() {
    check(RegOpenCurrentUser(KEY_ALL_ACCESS, &realUser) == ERROR_SUCCESS, "open test registry");
    path = L"Software\\ShibaKit-ViewTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64());
    DWORD disposition = 0;
    auto result = RegCreateKeyExW(realUser, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                                  &temporary, &disposition);
    if (result != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
      if (temporary) RegCloseKey(temporary);
      RegCloseKey(realUser);
      throw std::runtime_error("create isolated registry");
    }
    if (RegOverridePredefKey(HKEY_CURRENT_USER, temporary) != ERROR_SUCCESS) {
      RegCloseKey(temporary);
      RegDeleteTreeW(realUser, path.c_str());
      RegCloseKey(realUser);
      throw std::runtime_error("redirect isolated registry");
    }
  }
  ~IsolatedSettings() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(temporary);
    RegDeleteTreeW(realUser, path.c_str());
    RegCloseKey(realUser);
  }
  void set(navigation::Action action) {
    DWORD value = DWORD(action);
    check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"BlankDoubleClick",
                          REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS, "store test action");
  }
};
struct FakeView final : IShellView {
  ULONG refs = 1;
  unsigned folderQueries = 0;
  HWND window = nullptr;
  HRESULT windowResult = S_OK;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (iid == IID_IFolderView) ++folderQueries;
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IOleWindow && iid != IID_IShellView)
      return E_NOINTERFACE;
    *result = static_cast<IShellView *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *result) override {
    if (!result) return E_POINTER;
    *result = SUCCEEDED(windowResult) ? window : nullptr;
    return windowResult;
  }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UIActivate(UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Refresh() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateViewWindow(IShellView *, LPCFOLDERSETTINGS, IShellBrowser *,
                                              RECT *, HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DestroyViewWindow() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurrentInfo(LPFOLDERSETTINGS) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SaveViewState() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SelectItem(PCUITEMID_CHILD, SVSIF) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetItemObject(UINT, REFIID, void **) override { return E_NOTIMPL; }
};
struct FakeBrowser final : IShellBrowser {
  ULONG refs = 1;
  FakeView *active = nullptr;
  HRESULT queryResult = S_OK, browseResult = S_OK;
  int queries = 0, navigations = 0;
  UINT flags = 0;
  PCUIDLIST_RELATIVE pidl = nullptr;
  std::function<void()> onQuery;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IOleWindow && iid != IID_IShellBrowser)
      return E_NOINTERFACE;
    *result = static_cast<IShellBrowser *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetMenuSB(HMENU, HOLEMENU, HWND) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RemoveMenusSB(HMENU) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetStatusTextSB(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableModelessSB(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TranslateAcceleratorSB(MSG *, WORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BrowseObject(PCUIDLIST_RELATIVE value, UINT valueFlags) override {
    ++navigations; pidl = value; flags = valueFlags; return browseResult;
  }
  HRESULT STDMETHODCALLTYPE GetViewStateStream(DWORD, IStream **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetControlWindow(UINT, HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SendControlMsg(UINT, UINT, WPARAM, LPARAM, LRESULT *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView **result) override {
    ++queries;
    if (onQuery) onQuery();
    if (!result) return E_POINTER;
    *result = SUCCEEDED(queryResult) ? active : nullptr;
    if (*result) (*result)->AddRef();
    return queryResult;
  }
  HRESULT STDMETHODCALLTYPE OnViewWindowActive(IShellView *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override { return E_NOTIMPL; }
};
void browseContracts() {
  using navigation::Action;
  FakeView view;
  view.window = reinterpret_cast<HWND>(1);
  FakeBrowser browser;
  browser.active = &view;
  check(viewnav::browse(nullptr, view.window, Action::Parent) == E_INVALIDARG, "null browser");
  check(viewnav::browse(&browser, nullptr, Action::Parent) == E_INVALIDARG, "null expected view");
  check(viewnav::browse(&browser, view.window, Action::Off) == E_INVALIDARG, "off rejects browse");
  check(viewnav::browse(&browser, view.window, Action(99)) == E_INVALIDARG, "unknown action");
  check(browser.queries == 0 && browser.navigations == 0, "disabled never queries browser");
  check(viewnav::browse(&browser, view.window, Action::Parent) == S_OK, "parent succeeds");
  check(browser.pidl == nullptr && browser.flags == (SBSP_SAMEBROWSER | SBSP_PARENT), "parent flags");
  check(viewnav::browse(&browser, view.window, Action::Back) == S_OK, "back succeeds");
  check(browser.pidl == nullptr && browser.flags == (SBSP_SAMEBROWSER | SBSP_NAVIGATEBACK), "back flags");
  check(view.refs == 1, "active view reference released");
  check(FAILED(viewnav::browse(&browser, reinterpret_cast<HWND>(2), Action::Parent)), "stale tab rejected");
  check(browser.navigations == 2, "stale tab never navigates");
  browser.queryResult = E_ACCESSDENIED;
  check(viewnav::browse(&browser, view.window, Action::Parent) == E_ACCESSDENIED, "query failure preserved");
  browser.queryResult = S_OK;
  browser.active = nullptr;
  check(FAILED(viewnav::browse(&browser, view.window, Action::Parent)), "null active view rejected");
  browser.active = &view;
  view.windowResult = E_FAIL;
  check(viewnav::browse(&browser, view.window, Action::Parent) == E_FAIL, "GetWindow failure preserved");
  check(browser.navigations == 2 && view.refs == 1, "failed view leaves no navigation or reference");
  view.windowResult = S_OK;
  browser.browseResult = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  check(viewnav::browse(&browser, view.window, Action::Back) == browser.browseResult, "browse failure preserved");
}
void pump() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}
bool waitForWorkers(long count) {
  const auto until = GetTickCount64() + 3000;
  do {
    pump();
    if (viewnav::activeQueryWorkers() == count) return true;
    MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  } while (GetTickCount64() < until);
  return viewnav::activeQueryWorkers() == count;
}
struct Window {
  HWND handle = nullptr;
  explicit Window(HWND parent = nullptr, const wchar_t *windowClass = L"STATIC") {
    handle = CreateWindowExW(0, windowClass, L"ShibaKit isolated view test", parent ? WS_CHILD : WS_OVERLAPPED,
                              0, 0, 100, 100, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(handle != nullptr, "create hidden fixture window");
  }
  ~Window() { if (handle && IsWindow(handle)) DestroyWindow(handle); }
};
struct Attached {
  viewnav::Controller *value = nullptr;
  ~Attached() { if (value) viewnav::detach(value); }
  void reset() { auto old = value; value = nullptr; viewnav::detach(old); }
};
struct ThreadFrame {
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HWND window = nullptr;
  DWORD threadId = 0;
  std::thread thread;
  ThreadFrame() {
    if (!ready) throw std::runtime_error("create thread ready event");
    thread = std::thread([this] {
      threadId = GetCurrentThreadId();
      window = CreateWindowExW(0, L"STATIC", L"Foreign UI thread fixture", WS_OVERLAPPED,
                                0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      SetEvent(ready);
      MSG message{};
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      if (window) DestroyWindow(window);
    });
    WaitForSingleObject(ready, INFINITE);
  }
  ~ThreadFrame() {
    PostThreadMessageW(threadId, WM_QUIT, 0, 0);
    thread.join();
    CloseHandle(ready);
  }
};
void lifecycle(IsolatedSettings &settings) {
  using navigation::Action;
  constexpr wchar_t property[] = L"ShibaKit.Navigation.ViewOwner.v2";
  const UINT settingsMessage = RegisterWindowMessageW(L"ShibaKit.Navigation.SettingsChanged.v2");
  const UINT shutdownMessage = RegisterWindowMessageW(L"ShibaKit.Navigation.Shutdown.v2");
  Window frame;
  Window surface(frame.handle);
  FakeView view;
  view.window = surface.handle;
  FakeBrowser browser;
  browser.active = &view;
  Attached controller;
  check(viewnav::activeControllers() == 0, "initial controllers empty");
  check(!viewnav::attach(nullptr, &browser), "attach null frame");
  check(!viewnav::attach(frame.handle, nullptr), "attach null browser");
  settings.set(Action::Off);
  controller.value = viewnav::attach(frame.handle, &browser);
  check(controller.value && viewnav::activeControllers() == 1, "off controller retains settings receiver");
  check(browser.queries == 0 && viewnav::activeQueryWorkers() == 0, "off does not start worker");
  check(browser.refs == 2, "controller retains browser");
  controller.reset();
  check(viewnav::activeControllers() == 0 && browser.refs == 1, "detach releases browser and controller");
  DWORD enabled = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled));
  controller.value = viewnav::attach(frame.handle, &browser);
  check(controller.value && viewnav::hasView(controller.value), "quick folders attaches when double click is off");
  check(viewnav::activeQueryWorkers() == 0, "quick folders does not start blank-hit worker");
  enabled = 0;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled));
  SendMessageW(frame.handle, settingsMessage, 0, 0);
  check(!viewnav::hasView(controller.value), "all features off removes view subclass");
  controller.reset();
  settings.set(Action::Parent);
  controller.value = viewnav::attach(frame.handle, &browser);
  check(controller.value && GetPropW(surface.handle, property), "enabled attaches view");
  check(waitForWorkers(1), "enabled creates query worker");
  for (int i = 0; i < 5; ++i) {
    settings.set(Action::Off);
    SendMessageW(frame.handle, settingsMessage, 0, 0);
    check(!GetPropW(surface.handle, property), "off removes view subclass marker");
    settings.set(Action::Back);
    SendMessageW(frame.handle, settingsMessage, 0, 0);
    check(GetPropW(surface.handle, property) != nullptr, "reenable attaches existing view");
  }
  settings.set(Action::Off);
  SendMessageW(frame.handle, settingsMessage, 0, 0);
  check(waitForWorkers(0), "off drains all query workers");
  settings.set(Action::Parent);
  SendMessageW(frame.handle, settingsMessage, 0, 0);
  SendMessageW(frame.handle, shutdownMessage, 0, 0);
  check(!GetPropW(surface.handle, property), "shutdown detaches surfaces");
  check(waitForWorkers(0), "shutdown releases worker");
  SendMessageW(frame.handle, settingsMessage, 0, 0);
  check(GetPropW(surface.handle, property) != nullptr, "settings can resume installed extension");
  DestroyWindow(surface.handle);
  view.window = nullptr;
  pump();
  Window replacement(frame.handle);
  view.window = replacement.handle;
  viewnav::refresh(controller.value, &browser);
  check(GetPropW(replacement.handle, property) != nullptr, "navigation replaces destroyed view");
  DestroyWindow(frame.handle);
  pump();
  check(waitForWorkers(0), "frame destruction releases worker");
  check(viewnav::activeControllers() == 1, "host can detach controller after window destruction");
  controller.reset();
  check(viewnav::activeControllers() == 0 && browser.refs == 1 && view.refs == 1,
        "post-destroy detach releases all COM references");
}
void reentrantDetach(IsolatedSettings &settings) {
  settings.set(navigation::Action::Parent);
  Window frame, surface(frame.handle);
  FakeView view;
  view.window = surface.handle;
  FakeBrowser browser;
  browser.active = &view;
  Attached controller;
  controller.value = viewnav::attach(frame.handle, &browser);
  check(controller.value != nullptr, "reentrant test attaches");
  browser.onQuery = [&] { controller.reset(); };
  viewnav::refresh(controller.value, &browser);
  browser.onQuery = {};
  check(!GetPropW(surface.handle, L"ShibaKit.Navigation.ViewOwner.v2"), "reentrant detach leaves no surface callback");
  check(viewnav::activeControllers() == 0 && browser.refs == 1, "reentrant detach deletes once");
  check(waitForWorkers(0), "reentrant detach releases worker");
  SendMessageW(frame.handle, WM_ACTIVATE, WA_ACTIVE, 0);
  pump();
}
void tabIsolation(IsolatedSettings &settings) {
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"ShellTabWindowClass";
  check(RegisterClassW(&cls) != 0, "register isolated tab host class");
  {
    Window frame;
    Window firstTab(frame.handle, cls.lpszClassName), secondTab(frame.handle, cls.lpszClassName);
    Window firstSurface(firstTab.handle), secondSurface(secondTab.handle), foreign;
    check(viewnav::attachmentWindow(nullptr, firstTab.handle, firstSurface.handle) == nullptr,
          "attachment requires root frame");
    check(viewnav::attachmentWindow(frame.handle, frame.handle, firstSurface.handle) == firstTab.handle,
          "shared browser frame resolves first native tab");
    check(viewnav::attachmentWindow(frame.handle, frame.handle, secondSurface.handle) == secondTab.handle,
          "shared browser frame resolves second native tab");
    check(viewnav::attachmentWindow(frame.handle, firstTab.handle, foreign.handle) == nullptr,
          "foreign view never attaches");
    check(viewnav::attachmentWindow(frame.handle, firstTab.handle, nullptr) == firstTab.handle,
          "browser tab can await initial view");
    FakeView firstView, secondView;
    firstView.window = firstSurface.handle;
    secondView.window = secondSurface.handle;
    FakeBrowser firstBrowser, secondBrowser;
    firstBrowser.active = &firstView;
    secondBrowser.active = &secondView;
    settings.set(navigation::Action::Parent);
    Attached first, second;
    first.value = viewnav::attach(firstTab.handle, &firstBrowser);
    second.value = viewnav::attach(secondTab.handle, &secondBrowser);
    check(first.value && second.value && viewnav::activeControllers() == 2,
          "two tabs in one frame attach independently");
    check(viewnav::hasView(first.value) && viewnav::hasView(second.value),
          "both browser views are attached");
    check(!viewnav::attach(firstTab.handle, &firstBrowser), "same tab duplicate rejected");
    check(GetPropW(firstTab.handle, L"ShibaKit.Navigation.Controller.v3") &&
          GetPropW(secondTab.handle, L"ShibaKit.Navigation.Controller.v3"),
          "settings can discover both tab receivers");
    check(waitForWorkers(1), "two tabs share one query worker");
    // A top-level browser service must never retarget a tab controller to the
    // sibling view merely because the visible native tab changed.
    firstBrowser.active = &secondView;
    viewnav::refresh(first.value, &firstBrowser);
    check(!viewnav::hasView(first.value) && viewnav::hasView(second.value),
          "cross-tab active view rejected without disturbing its owner");
    firstBrowser.active = &firstView;
    viewnav::refresh(first.value, &firstBrowser);
    check(viewnav::hasView(first.value), "original tab can reattach");
    const UINT settingsMessage = RegisterWindowMessageW(L"ShibaKit.Navigation.SettingsChanged.v2");
    settings.set(navigation::Action::Off);
    SendMessageW(firstTab.handle, settingsMessage, 0, 0);
    SendMessageW(secondTab.handle, settingsMessage, 0, 0);
    check(!viewnav::hasView(first.value) && !viewnav::hasView(second.value),
          "off reaches each tab receiver");
    check(waitForWorkers(0), "all tab queries stop when disabled");
    settings.set(navigation::Action::Back);
    SendMessageW(firstTab.handle, settingsMessage, 0, 0);
    SendMessageW(secondTab.handle, settingsMessage, 0, 0);
    check(viewnav::hasView(first.value) && viewnav::hasView(second.value),
          "existing tabs resume independently");
    first.reset();
    check(!GetPropW(firstTab.handle, L"ShibaKit.Navigation.Controller.v3") &&
          viewnav::hasView(second.value), "closing first tab preserves second tab");
    second.reset();
    check(waitForWorkers(0) && viewnav::activeControllers() == 0,
          "tab teardown releases all controllers and worker");
  }
  {
    ThreadFrame frame;
    check(frame.window && GetWindowThreadProcessId(frame.window, nullptr) != GetCurrentThreadId(),
          "fixture root belongs to another UI thread");
    Window tab(frame.window, cls.lpszClassName), surface(tab.handle);
    check(viewnav::attachmentWindow(frame.window, frame.window, surface.handle) == tab.handle,
          "different-thread frame resolves same-thread native tab");
    check(!viewnav::attachmentWindow(frame.window, frame.window, nullptr),
          "unready different-thread frame never gets subclassed");
    FakeView view;
    view.window = surface.handle;
    FakeBrowser browser;
    browser.active = &view;
    settings.set(navigation::Action::Parent);
    Attached controller;
    controller.value = viewnav::attach(tab.handle, &browser);
    check(viewnav::hasView(controller.value), "tab attaches without subclassing foreign-thread frame");
    controller.reset();
    check(waitForWorkers(0), "cross-thread fixture releases query worker");
  }
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
void middleInput(IsolatedSettings &settings) {
  settings.set(navigation::Action::Off);
  DWORD enabled = 1, dialogs = 0;
  check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled)) == ERROR_SUCCESS &&
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersInDialogs", REG_DWORD, &dialogs, sizeof(dialogs)) == ERROR_SUCCESS,
        "enable isolated Explorer-only middle menu");
  Window frame, other, surface(frame.handle);
  FakeView view; view.window = surface.handle;
  FakeBrowser browser; browser.active = &view;
  Attached controller; controller.value = viewnav::attach(frame.handle, &browser);
  check(controller.value && viewnav::hasView(controller.value), "attach isolated middle input surface");
  struct InputScope {
    InputScope(HWND frame, HWND view) { isolatedMiddle = true; middleForeground = frame; middleView = view; middleModifier = 0; menuLaunches = 0; }
    ~InputScope() { isolatedMiddle = false; middleForeground = middleView = nullptr; middleModifier = 0; }
  } input(frame.handle, surface.handle);
  auto send = [&](UINT message, int x = 20, int y = 20) {
    SendMessageW(surface.handle, message, message == WM_MBUTTONUP ? 0 : MK_MBUTTON, MAKELPARAM(x, y));
  };
  send(WM_MBUTTONDOWN); send(WM_MBUTTONUP);
  send(WM_MBUTTONDBLCLK); send(WM_MBUTTONUP);
  check(menuLaunches == 2, "native middle double-click produces a second independent menu request");
  const int jitter = 7;
  send(WM_MBUTTONDOWN);
  SendMessageW(surface.handle, WM_MOUSEWHEEL, MAKEWPARAM(MK_MBUTTON, WHEEL_DELTA), MAKELPARAM(20, 20));
  send(WM_MOUSEMOVE, 20 + jitter);
  send(WM_MBUTTONUP, 20 + jitter);
  check(menuLaunches == 3, "wheel notch and 7px press jitter during middle-click still open the menu");
  const int drag = GetSystemMetrics(SM_CXDRAG) + 10;
  send(WM_MBUTTONDOWN); send(WM_MOUSEMOVE, 20 + drag); send(WM_MOUSEMOVE); send(WM_MBUTTONUP);
  send(WM_MBUTTONDOWN); send(WM_MBUTTONUP, 20 + drag);
  check(menuLaunches == 3, "large movement cancels even after returning or without move messages");
  for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) {
    middleModifier = key; send(WM_MBUTTONDOWN); send(WM_MBUTTONUP); middleModifier = 0;
    send(WM_MBUTTONDOWN); middleModifier = key; send(WM_MBUTTONUP); middleModifier = 0;
  }
  check(menuLaunches == 3, "middle click rejects modifiers at press and release");
  send(WM_MBUTTONDOWN); middleForeground = other.handle; send(WM_MBUTTONUP); middleForeground = frame.handle;
  check(menuLaunches == 3, "switching foreground during click cancels Explorer menu request");
  send(WM_MBUTTONDOWN); SendMessageW(surface.handle, WM_CANCELMODE, 0, 0); send(WM_MBUTTONUP);
  check(menuLaunches == 3, "cancel mode discards pending middle click");
  send(WM_MBUTTONDOWN); enabled = 0;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled));
  send(WM_MBUTTONUP); check(menuLaunches == 3, "feature disabled between press and release does not launch");
  enabled = 1; dialogs = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled));
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersInDialogs", REG_DWORD, &dialogs, sizeof(dialogs));
  send(WM_MBUTTONDOWN); send(WM_MBUTTONUP);
  check(menuLaunches == 3, "dialog helper mode prevents duplicate launch from Explorer extension");
  controller.reset(); enabled = 0;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled));
}
struct WheelFixture {
  static inline unsigned requests = 0, actions = 0, fileScrolls = 0, parentScrolls = 0;
  static inline WPARAM receivedKeys = 0;
  static inline LPARAM receivedPoint = 0;
  static LRESULT CALLBACK bandProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == tabs::wheelInputMessage) {
      ++requests; receivedKeys = w; receivedPoint = l;
      RECT tab{100, 200, 160, 240}; POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (PtInRect(&tab, point)) { ++actions; return TRUE; }
      return FALSE;
    }
    return DefWindowProcW(window, message, w, l);
  }
  static LRESULT CALLBACK viewProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_MOUSEWHEEL) { ++fileScrolls; return SendMessageW(GetParent(window), message, w, l); }
    return DefWindowProcW(window, message, w, l);
  }
  static LRESULT CALLBACK frameProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_MOUSEWHEEL) { ++parentScrolls; return 71; }
    return DefWindowProcW(window, message, w, l);
  }
};
void tabWheelInput(IsolatedSettings &settings) {
  if (!navigation::tabsSupported()) return;
  settings.set(navigation::Action::Off);
  DWORD disabled = 0, enabled = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFoldersEnabled", REG_DWORD, &disabled, sizeof(disabled));
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &disabled, sizeof(disabled));
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"ShibaKit.Test.WheelFrame"; cls.lpfnWndProc = WheelFixture::frameProc;
  WNDCLASSW viewClass = cls, bandClass = cls;
  viewClass.lpszClassName = L"ShibaKit.Test.WheelView"; viewClass.lpfnWndProc = WheelFixture::viewProc;
  bandClass.lpszClassName = L"ShibaKit.ExplorerTabs.Band.v1"; bandClass.lpfnWndProc = WheelFixture::bandProc;
  check(RegisterClassW(&cls) && RegisterClassW(&viewClass) && RegisterClassW(&bandClass), "register hidden wheel-routing fixtures");
  {
    Window frame(nullptr, cls.lpszClassName), other(nullptr, cls.lpszClassName), surface(frame.handle, viewClass.lpszClassName),
           band(frame.handle, bandClass.lpszClassName), foreignBand(other.handle, bandClass.lpszClassName), ordinary(frame.handle);
    struct VisibilityScope { ~VisibilityScope() { wheelVisibleBand = nullptr; wheelBandVisible = false; } } visibility;
    wheelVisibleBand = band.handle; wheelBandVisible = true;
    WheelFixture::requests = WheelFixture::actions = WheelFixture::fileScrolls = WheelFixture::parentScrolls = 0;
    FakeView view; view.window = surface.handle; FakeBrowser browser; browser.active = &view;
    Attached controller; controller.value = viewnav::attach(frame.handle, &browser);
    check(controller.value && !viewnav::hasView(controller.value), "all optional features off omit wheel view subclasses");
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
    const UINT settingsChanged = RegisterWindowMessageW(L"ShibaKit.Navigation.SettingsChanged.v2");
    SendMessageW(frame.handle, settingsChanged, 0, 0);
    check(viewnav::hasView(controller.value) && viewnav::activeQueryWorkers() == 0 && view.folderQueries == 0,
          "tabs-only mode attaches wheel surfaces without UIA worker or folder history lookup");
    SetPropW(frame.handle, tabs::windowProperty, band.handle);
    const WPARAM keys = MAKEWPARAM(0, WHEEL_DELTA); const LPARAM hit = MAKELPARAM(120, 220), miss = MAKELPARAM(20, 20);
    check(SendMessageW(surface.handle, WM_MOUSEWHEEL, keys, hit) == TRUE && WheelFixture::requests == 1 &&
          WheelFixture::actions == 1 && !WheelFixture::fileScrolls && !WheelFixture::parentScrolls &&
          WheelFixture::receivedKeys == keys && WheelFixture::receivedPoint == hit,
          "focused file view forwards exact screen point and consumes a tab hit once before native parent propagation");
    check(SendMessageW(frame.handle, WM_MOUSEWHEEL, keys, hit) == TRUE && WheelFixture::actions == 2 && WheelFixture::requests == 2,
          "frame delivery also routes one wheel action to the same band");
    check(SendMessageW(surface.handle, WM_MOUSEWHEEL, keys, miss) == 71 && WheelFixture::actions == 2 &&
          WheelFixture::fileScrolls == 1 && WheelFixture::parentScrolls == 1,
          "point outside tabs preserves ordinary file-list scrolling and parent propagation");
    auto refused = [&] {
      const auto beforeRequests = WheelFixture::requests, beforeScrolls = WheelFixture::fileScrolls;
      check(SendMessageW(surface.handle, WM_MOUSEWHEEL, keys, hit) == 71 && WheelFixture::requests == beforeRequests &&
            WheelFixture::fileScrolls == beforeScrolls + 1, "unusable band is never sent wheel input and native scroll survives");
    };
    wheelBandVisible = false; refused(); wheelBandVisible = true;
    EnableWindow(band.handle, FALSE); refused(); EnableWindow(band.handle, TRUE);
    wheelVisibleBand = foreignBand.handle; SetPropW(frame.handle, tabs::windowProperty, foreignBand.handle); refused();
    wheelVisibleBand = ordinary.handle; SetPropW(frame.handle, tabs::windowProperty, ordinary.handle); refused();
    SetPropW(frame.handle, tabs::windowProperty, reinterpret_cast<HANDLE>(UINT_PTR(1))); refused();
    wheelVisibleBand = band.handle; SetPropW(frame.handle, tabs::windowProperty, band.handle);
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &disabled, sizeof(disabled));
    refused(); SendMessageW(frame.handle, settingsChanged, 0, 0);
    check(!viewnav::hasView(controller.value) && viewnav::activeQueryWorkers() == 0 && view.folderQueries == 0,
          "turning last feature off releases wheel subclasses without starting background work");
    RemovePropW(frame.handle, tabs::windowProperty); controller.reset();
  }
  UnregisterClassW(bandClass.lpszClassName, cls.hInstance); UnregisterClassW(viewClass.lpszClassName, cls.hInstance); UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
} // namespace
int main() {
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(init)) return 1;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&controls);
  int result = 0;
  try {
    IsolatedSettings settings;
    browseContracts();
    lifecycle(settings);
    reentrantDetach(settings);
    tabIsolation(settings);
    middleInput(settings);
    tabWheelInput(settings);
    std::cout << "PASS " << checks << " shell navigation, settings, worker, controller lifecycle, middle-input and tab-wheel checks\n";
  } catch (const std::exception &e) {
    std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
    result = 1;
  }
  CoUninitialize();
  return result;
}
