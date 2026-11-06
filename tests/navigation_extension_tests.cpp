#include "../src/core.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <iostream>
#include <functional>

// Compile the same host implementation into this test only so a fake Explorer
// site can exercise COM reentrancy. The separately loaded production DLL below
// still uses the real process check and must reject this test executable.
static DWORD testHostProcessPath(HMODULE instance, LPWSTR value, DWORD capacity) {
  if (instance)
    return GetModuleFileNameW(instance, value, capacity);
  wchar_t windows[MAX_PATH]{};
  UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
  std::wstring path(windows, length);
  path += L"\\explorer.exe";
  if (path.size() >= capacity)
    return capacity;
  memcpy(value, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
  return static_cast<DWORD>(path.size());
}
#define GetModuleFileNameW testHostProcessPath
#include "../src/navigation_extension.cpp"
#undef GetModuleFileNameW

using Microsoft::WRL::ComPtr;

static void require(bool condition, const char *message) {
  if (!condition)
    throw message;
}

namespace {
struct FakeView final : IShellView {
  ULONG refs = 1;
  HWND window = nullptr;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
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
    *result = window; return S_OK;
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
  HWND window = nullptr;
  FakeView *active = nullptr;
  int queries = 0;
  std::function<void(int)> onQuery;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IOleWindow && iid != IID_IShellBrowser)
      return E_NOINTERFACE;
    *result = static_cast<IShellBrowser *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *result) override {
    if (!result) return E_POINTER;
    *result = window; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetMenuSB(HMENU, HOLEMENU, HWND) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RemoveMenusSB(HMENU) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetStatusTextSB(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableModelessSB(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TranslateAcceleratorSB(MSG *, WORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BrowseObject(PCUIDLIST_RELATIVE, UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetViewStateStream(DWORD, IStream **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetControlWindow(UINT, HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SendControlMsg(UINT, UINT, WPARAM, LPARAM, LRESULT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView **result) override {
    ++queries;
    if (onQuery) onQuery(queries);
    if (!result) return E_POINTER;
    *result = active;
    if (*result) (*result)->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnViewWindowActive(IShellView *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override { return E_NOTIMPL; }
};
struct Fixture {
  HWND frame = nullptr, surface = nullptr;
  FakeView view;
  FakeBrowser browser;
  Fixture() {
    frame = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"CabinetWClass", L"Isolated host test",
                            WS_OVERLAPPED | WS_VISIBLE, -30000, -30000, 20, 20, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    require(frame != nullptr, "create isolated Explorer frame");
    surface = CreateWindowExW(0, L"STATIC", L"Isolated Shell view", WS_CHILD,
                              0, 0, 10, 10, frame, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(surface != nullptr, "create isolated Shell view");
    view.window = surface;
    browser.window = frame;
    browser.active = &view;
  }
  ~Fixture() { DestroyWindow(frame); }
};
struct IsolatedSettings {
  HKEY realUser = nullptr, temporary = nullptr;
  std::wstring path;
  IsolatedSettings() {
    require(RegOpenCurrentUser(KEY_ALL_ACCESS, &realUser) == ERROR_SUCCESS, "open test settings");
    path = L"Software\\FolderNote-HostTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(GetTickCount64());
    require(RegCreateKeyExW(realUser, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                            &temporary, nullptr) == ERROR_SUCCESS, "create isolated host settings");
    require(RegOverridePredefKey(HKEY_CURRENT_USER, temporary) == ERROR_SUCCESS, "isolate host settings");
    DWORD action = 1;
    require(RegSetKeyValueW(temporary, L"Software\\FolderNote", L"BlankDoubleClick", REG_DWORD,
                            &action, sizeof(action)) == ERROR_SUCCESS, "enable isolated navigation");
  }
  ~IsolatedSettings() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(temporary);
    RegDeleteTreeW(realUser, path.c_str());
    RegCloseKey(realUser);
  }
};
void pumpHostMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}
void reentrantHostAttachment() {
  IsolatedSettings settings;
  WNDCLASSW cls{};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"CabinetWClass";
  require(RegisterClassW(&cls) != 0, "register isolated Explorer class");
  for (int queryStage : {1, 2}) {
    for (bool replace : {false, true}) {
      Fixture original, replacement;
      ComPtr<IObjectWithSite> host;
      host.Attach(new BrowserHost());
      original.browser.onQuery = [&](int calls) {
        if (calls == queryStage)
          require(SUCCEEDED(host->SetSite(replace ? &replacement.browser : nullptr)),
                  "reentrant SetSite succeeds");
      };
      require(SUCCEEDED(host->SetSite(&original.browser)), "attach initial fake browser");
      pumpHostMessages();
      require(original.browser.queries == queryStage, "reentrant attachment stage reached");
      require(!GetPropW(original.surface, L"FolderNote.Navigation.ViewOwner.v2") &&
              !GetPropW(original.frame, L"FolderNote.Navigation.Controller.v3") &&
              !GetPropW(original.frame, ownerProperty), "stale attachment leaves no callbacks or owner");
      ComPtr<IShellBrowser> retained;
      HRESULT result = host->GetSite(IID_PPV_ARGS(&retained));
      if (replace) {
        require(SUCCEEDED(result) && retained.Get() == &replacement.browser,
                "newer site survives old update completion");
        require(viewnav::activeControllers() == 1 &&
                GetPropW(replacement.surface, L"FolderNote.Navigation.ViewOwner.v2"),
                "only replacement browser receives navigation controller");
      } else {
        require(result == E_FAIL && !retained && viewnav::activeControllers() == 0,
                "disconnect remains empty after stale attach returns");
      }
      retained.Reset();
      host->SetSite(nullptr);
      host.Reset();
      require(viewnav::activeControllers() == 0 && original.browser.refs == 1 &&
              replacement.browser.refs == 1 && original.view.refs == 1 && replacement.view.refs == 1,
              "reentrant host releases controllers and COM references");
    }
  }
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
void deferredHostRecovery() {
  if (!navigation::tabsSupported()) return;
  IsolatedSettings settings;
  DWORD tabsOff = 0;
  require(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerTabsEnabled",
                          REG_DWORD, &tabsOff, sizeof(tabsOff)) == ERROR_SUCCESS,
          "disable tab rendering in isolated BHO lifecycle tests");
  WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"CabinetWClass";
  require(RegisterClassW(&cls) != 0, "register deferred host test frame");
  {
    Fixture fixture;
    ShowWindow(fixture.frame, SW_HIDE);
    ComPtr<IObjectWithSite> host; host.Attach(new BrowserHost());
    require(SUCCEEDED(host->SetSite(&fixture.browser)), "attach initially hidden browser");
    pumpHostMessages();
    HWND helper = nullptr, candidate = nullptr;
    while ((candidate = FindWindowExW(HWND_MESSAGE, candidate, L"STATIC", L"FolderNote Navigation Host"))) {
      DWORD process = 0;
      if (GetWindowThreadProcessId(candidate, &process) == GetCurrentThreadId() &&
          process == GetCurrentProcessId()) { helper = candidate; break; }
    }
    require(helper != nullptr, "locate only this test process's startup helper");
    // Exhaust the bounded startup attempts deterministically, without sleeping
    // or sending any message to the user's actual Explorer windows.
    for (unsigned attempt = 0; attempt < 20; ++attempt)
      SendMessageW(helper, WM_TIMER, startupTimer, 0);
    require(viewnav::activeControllers() == 0, "hidden browser remains unattached after startup budget");
    ShowWindow(fixture.frame, SW_SHOWNOACTIVATE);
    pumpHostMessages();
    require(viewnav::activeControllers() == 1 &&
            GetPropW(fixture.surface, L"FolderNote.Navigation.ViewOwner.v2"),
            "show event recovers browser after exhausted startup retries");
    host->SetSite(nullptr); host.Reset();
    require(viewnav::activeControllers() == 0 && fixture.browser.refs == 1 && fixture.view.refs == 1,
            "recovered host releases callbacks and browser references");
  }
  {
    Fixture fixture;
    FakeBrowser duplicate; duplicate.window = fixture.frame; duplicate.active = &fixture.view;
    ComPtr<IObjectWithSite> first, second;
    first.Attach(new BrowserHost()); second.Attach(new BrowserHost());
    require(SUCCEEDED(first->SetSite(&fixture.browser)), "attach first frame host");
    pumpHostMessages();
    require(SUCCEEDED(second->SetSite(&duplicate)), "attach duplicate frame host");
    pumpHostMessages();
    require(viewnav::activeControllers() == 1, "same frame hosts share one navigation controller");
    const auto firstQueries = fixture.browser.queries, secondQueries = duplicate.queries;
    const auto sync = RegisterWindowMessageW(L"FolderNote.Navigation.SyncTabs.v1");
    require(SendMessageW(fixture.frame, sync, 0, 0) == 1 &&
            fixture.browser.queries > firstQueries && duplicate.queries > secondQueries,
            "synchronous settings reach every frame subclass and aggregate acknowledgment");
    first->SetSite(nullptr); first.Reset();
    require(SendMessageW(fixture.frame, sync, 0, 0) == 1 && viewnav::activeControllers() == 1,
            "remaining host can attach after original owner disconnects");
    second->SetSite(nullptr); second.Reset();
    require(viewnav::activeControllers() == 0 && fixture.browser.refs == 1 && duplicate.refs == 1 &&
            fixture.view.refs == 1, "duplicate hosts release all callbacks and references");
  }
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
} // namespace

int wmain() {
  HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized))
    return 1;
  // Factory and attachment diagnostics must never replace the user's live
  // Explorer diagnostics while exercising the actual production DLL.
  struct Registry {
    HKEY real = nullptr, temporary = nullptr; std::wstring path;
    Registry() {
      GUID id{}; CoCreateGuid(&id); wchar_t token[40]{}; StringFromGUID2(id, token, ARRAYSIZE(token));
      path = std::wstring(L"Software\\FolderNoteExtensionTests\\") + token;
      require(RegOpenCurrentUser(KEY_ALL_ACCESS, &real) == ERROR_SUCCESS &&
          RegCreateKeyExW(real, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &temporary, nullptr) == ERROR_SUCCESS &&
          RegOverridePredefKey(HKEY_CURRENT_USER, temporary) == ERROR_SUCCESS, "isolate extension diagnostics");
    }
    ~Registry() { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(temporary); RegDeleteTreeW(real, path.c_str()); RegCloseKey(real); }
  } registry;
  HMODULE library =
      LoadLibraryW((fn::modulePath().parent_path() / L"FolderNoteNavigation.dll").c_str());
  if (!library) {
    CoUninitialize();
    return 1;
  }
  int result = 0;
  try {
    auto get = reinterpret_cast<HRESULT(__stdcall *)(REFCLSID, REFIID, void **)>(
        GetProcAddress(library, "DllGetClassObject"));
    auto unload =
        reinterpret_cast<HRESULT(__stdcall *)()>(GetProcAddress(library, "DllCanUnloadNow"));
    require(get && unload, "COM exports");
    require(unload() == S_OK, "initial unload state");

    GUID clsid{};
    require(SUCCEEDED(CLSIDFromString(L"{87F11DB4-74C9-4B54-9379-E26D75D18B82}", &clsid)),
            "class identifier");
    require(get(clsid, IID_IClassFactory, nullptr) == E_POINTER, "null factory output");
    void *unknown = reinterpret_cast<void *>(1);
    require(get(GUID_NULL, IID_IClassFactory, &unknown) == CLASS_E_CLASSNOTAVAILABLE && !unknown,
            "unknown class rejection");
    require(get(clsid, IID_IDispatch, &unknown) == E_NOINTERFACE && !unknown,
            "unsupported factory interface");
    require(unload() == S_OK, "failed queries release factory");

    ComPtr<IClassFactory> factory;
    require(SUCCEEDED(get(clsid, IID_PPV_ARGS(&factory))), "factory creation");
    require(unload() == S_FALSE, "factory holds module");
    require(factory->CreateInstance(nullptr, IID_IObjectWithSite, nullptr) == E_POINTER,
            "null instance output");
    require(factory->CreateInstance(factory.Get(), IID_IObjectWithSite, &unknown) ==
                    CLASS_E_NOAGGREGATION &&
                !unknown,
            "aggregation rejection");
    require(factory->CreateInstance(nullptr, IID_IDispatch, &unknown) == E_NOINTERFACE && !unknown,
            "unsupported host interface");

    ComPtr<IObjectWithSite> host;
    require(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&host))), "host creation");
    require(host->QueryInterface(IID_IUnknown, nullptr) == E_POINTER, "null query output");
    require(host->GetSite(IID_IUnknown, nullptr) == E_POINTER, "null site output");
    require(host->GetSite(IID_IUnknown, &unknown) == E_FAIL && !unknown, "empty site");
    require(host->SetSite(nullptr) == S_OK, "idempotent detach");
    // This executable is deliberately not explorer.exe. Loading the COM DLL must
    // never attach to a browser, desktop or file dialog in an arbitrary process.
    require(host->SetSite(factory.Get()) == E_ACCESSDENIED, "foreign process rejected");
    require(host->GetSite(IID_IUnknown, &unknown) == E_FAIL && !unknown,
            "rejected site not retained");
    require(host->SetSite(nullptr) == S_OK, "detach after rejected site");
    factory.Reset();
    require(unload() == S_FALSE, "host holds module after factory release");
    host.Reset();
    require(unload() == S_OK, "host releases module");

    require(SUCCEEDED(get(clsid, IID_PPV_ARGS(&factory))), "second factory creation");
    require(factory->LockServer(TRUE) == S_OK, "server lock");
    factory.Reset();
    require(unload() == S_FALSE, "server lock holds module");
    require(SUCCEEDED(get(clsid, IID_PPV_ARGS(&factory))), "unlock factory");
    require(factory->LockServer(FALSE) == S_OK, "server unlock");
    factory.Reset();
    require(unload() == S_OK, "unlocked module unloads");
    GUID tabClsid{};
    require(SUCCEEDED(CLSIDFromString(tabs::clsidText, &tabClsid)) && tabClsid == tabs::clsid, "registered tab GUID exactly matches factory GUID");
    require(get(tabClsid, IID_IDispatch, &unknown) == E_NOINTERFACE && !unknown, "tab factory rejects unsupported interface");
    DWORD code = 0, size = sizeof(code);
    require(RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"TabsLastFactoryResult", RRF_RT_REG_DWORD, nullptr, &code, &size) == ERROR_SUCCESS && code == DWORD(E_NOINTERFACE), "factory failures are recorded before CreateInstance");
    require(SUCCEEDED(get(tabClsid, IID_PPV_ARGS(&factory))), "production DLL supplies tab factory");
    ComPtr<IDeskBand2> band;
    require(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&band))), "production DLL creates tab band");
    ComPtr<IPersistStream> persist; ComPtr<IObjectWithSite> tabSite; ComPtr<IInputObject> input;
    require(SUCCEEDED(band.As(&persist)) && SUCCEEDED(band.As(&tabSite)) && SUCCEEDED(band.As(&input)), "production band supports all required hosting interfaces");
    GUID persisted{}; require(persist->GetClassID(&persisted) == S_OK && persisted == tabClsid, "band persistence reports registered class");
    size = sizeof(code);
    require(RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"TabsLastFactoryResult", RRF_RT_REG_DWORD, nullptr, &code, &size) == ERROR_SUCCESS && code == S_OK, "successful factory result recorded");
    require(tabSite->SetSite(factory.Get()) == E_ACCESSDENIED, "production band rejects foreign process attachment");
    factory.Reset(); require(unload() == S_FALSE, "tab COM references retain module");
    input.Reset(); tabSite.Reset(); persist.Reset(); band.Reset();
    require(unload() == S_OK, "tab factory and instance release cleanly");
    reentrantHostAttachment();
    deferredHostRecovery();
    std::cout << "PASS navigation extension COM interfaces, foreign-process rejection, reentrant site replacement, deferred recovery and shared-frame synchronization\n";
  } catch (const char *error) {
    std::cerr << "FAIL " << error << "\n";
    result = 1;
  }
  // Failed tests may leave a lock count, but no COM pointers escape their scope.
  FreeLibrary(library);
  CoUninitialize();
  return result;
}
