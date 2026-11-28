#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
#include <exdisp.h>
#include <exdispid.h>
#include <ocidl.h>
#include <shlguid.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <atomic>
#include <new>
#include <string>
#include <utility>
#include "navigation_view.h"
#include "explorer_tabs.h"
#include "tabs_window_gate.h"

// The browser creates this apartment-threaded COM object on its UI thread. No
// global hooks, remote thread injection or separate listener process are used.
namespace {
using Microsoft::WRL::ComPtr;
constexpr CLSID navigationClsid = {
    0x87f11db4, 0x74c9, 0x4b54, {0x93, 0x79, 0xe2, 0x6d, 0x75, 0xd1, 0x8b, 0x82}};
constexpr wchar_t ownerProperty[] = L"ShibaKit.Navigation.BrowserOwner.v2";
constexpr UINT refreshMessage = WM_APP + 0x442;
constexpr UINT quitMessage = WM_APP + 0x443;
constexpr UINT_PTR startupTimer = 1;
constexpr UINT_PTR helperSubclass = 0x464e4248;
HMODULE module;
std::atomic<long> objects{0};
std::atomic<long> serverLocks{0};

bool isExplorerProcess() {
  wchar_t image[MAX_PATH]{}, windows[MAX_PATH]{};
  DWORD length = GetModuleFileNameW(nullptr, image, MAX_PATH);
  UINT windowsLength = GetWindowsDirectoryW(windows, MAX_PATH);
  if (!length || length >= MAX_PATH || !windowsLength || windowsLength >= MAX_PATH)
    return false;
  std::wstring expected(windows, windowsLength);
  expected += L"\\explorer.exe";
  return _wcsicmp(image, expected.c_str()) == 0;
}

bool isFolderFrame(HWND frame) {
  if (!IsWindow(frame))
    return false;
  DWORD process = 0;
  GetWindowThreadProcessId(frame, &process);
  if (process != GetCurrentProcessId())
    return false;
  wchar_t name[64]{};
  if (!GetClassNameW(frame, name, ARRAYSIZE(name)))
    return false;
  return wcscmp(name, L"CabinetWClass") == 0 || wcscmp(name, L"ExploreWClass") == 0;
}

ComPtr<IShellBrowser> shellBrowser(IUnknown *site) {
  ComPtr<IShellBrowser> browser;
  if (!site)
    return browser;
  ComPtr<IServiceProvider> services;
  if (SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&services)))) {
    services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    if (!browser)
      services->QueryService(SID_SInPlaceBrowser, IID_PPV_ARGS(&browser));
    if (!browser)
      services->QueryService(IID_IShellBrowser, IID_PPV_ARGS(&browser));
  }
  if (!browser)
    site->QueryInterface(IID_PPV_ARGS(&browser));
  return browser;
}

ComPtr<IWebBrowser2> webBrowser(IUnknown *site) {
  ComPtr<IWebBrowser2> browser;
  if (!site)
    return browser;
  site->QueryInterface(IID_PPV_ARGS(&browser));
  if (!browser) {
    ComPtr<IServiceProvider> services;
    if (SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&services))))
      services->QueryService(IID_IWebBrowserApp, IID_PPV_ARGS(&browser));
  }
  return browser;
}

class BrowserHost;

// A separate sink avoids a host -> connection point -> host reference cycle.
// detach() first clears its owner, so even a failed Unadvise leaves an inert sink.
class BrowserEvents final : public IDispatch {
  std::atomic<ULONG> refs{1};
  BrowserHost *owner;

public:
  explicit BrowserEvents(BrowserHost *value) : owner(value) { ++objects; }
  ~BrowserEvents() { --objects; }
  void clearOwner() { owner = nullptr; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDispatch && iid != DIID_DWebBrowserEvents2)
      return E_NOINTERFACE;
    *result = static_cast<IDispatch *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *count) override {
    if (!count)
      return E_POINTER;
    *count = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **info) override {
    if (!info)
      return E_POINTER;
    *info = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override {
    return DISP_E_UNKNOWNNAME;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *result,
                                   EXCEPINFO *, UINT *) override;
};

class BrowserHost final : public IObjectWithSite {
  std::atomic<ULONG> refs{1};
  DWORD ownerThread = GetCurrentThreadId();
  ComPtr<IUnknown> site;
  ComPtr<IWebBrowser2> web;
  ComPtr<IConnectionPoint> connection;
  ComPtr<BrowserEvents> events;
  DWORD cookie = 0;
  HWND helper = nullptr;
  HWND host = nullptr;
  HWND tabsFrame = nullptr;
  bool tabsStopped = false;
  tabs::Host tabsHost;
  viewnav::Controller *controller = nullptr;
  unsigned startupAttempts = 0;
  bool refreshPending = false;
  unsigned long long generation = 0;

  void detachController() {
    auto *oldController = std::exchange(controller, nullptr);
    HWND oldHost = std::exchange(host, nullptr);
    if (oldHost && GetPropW(oldHost, ownerProperty) == this)
      RemovePropW(oldHost, ownerProperty);
    if (oldController)
      viewnav::detach(oldController);
  }

  void disconnect() {
    ++generation;
    // Clear published state before any COM release/Unadvise can pump messages.
    // A reentrant SetSite owns a newer generation and must not be reset by us.
    auto oldEvents = std::move(events);
    auto oldConnection = std::move(connection);
    auto oldWeb = std::move(web);
    auto oldSite = std::move(site);
    DWORD oldCookie = std::exchange(cookie, 0);
    HWND oldHelper = std::exchange(helper, nullptr);
    HWND oldTabsFrame = std::exchange(tabsFrame, nullptr);
    auto *oldController = std::exchange(controller, nullptr);
    HWND oldHost = std::exchange(host, nullptr);
    refreshPending = false;
    tabsStopped = false;
    if (oldEvents)
      oldEvents->clearOwner();
    if (oldHost && GetPropW(oldHost, ownerProperty) == this)
      RemovePropW(oldHost, ownerProperty);
    if (oldTabsFrame)
      RemoveWindowSubclass(oldTabsFrame, tabsFrameProc, reinterpret_cast<UINT_PTR>(this));
    // Removing the band can pump a new SetSite. Everything below this point
    // operates only on detached old state, never a newly attached controller.
    tabsHost.clear();
    if (oldHelper) {
      KillTimer(oldHelper, startupTimer);
      RemoveWindowSubclass(oldHelper, helperProc, helperSubclass);
      DestroyWindow(oldHelper);
    }
    if (oldController)
      viewnav::detach(oldController);
    if (oldConnection && oldCookie)
      oldConnection->Unadvise(oldCookie);
  }

  void connectEvents(unsigned long long expected) {
    auto currentWeb = web;
    if (!currentWeb)
      return;
    ComPtr<IConnectionPointContainer> container;
    ComPtr<IConnectionPoint> newConnection;
    if (FAILED(currentWeb.As(&container)) || generation != expected ||
        FAILED(container->FindConnectionPoint(DIID_DWebBrowserEvents2, &newConnection)) ||
        generation != expected)
      return;
    ComPtr<BrowserEvents> newEvents;
    newEvents.Attach(new (std::nothrow) BrowserEvents(this));
    DWORD newCookie = 0;
    if (!newEvents)
      return;
    HRESULT result = newConnection->Advise(newEvents.Get(), &newCookie);
    if (FAILED(result) || generation != expected) {
      newEvents->clearOwner();
      if (newCookie)
        newConnection->Unadvise(newCookie);
      return;
    }
    connection = std::move(newConnection);
    events = std::move(newEvents);
    cookie = newCookie;
  }

  void gateHiddenFrame(IShellBrowser *browser, IWebBrowser2 *currentWeb, unsigned long long expected) {
    HWND candidate = nullptr;
    if (browser) browser->GetWindow(&candidate);
    if (generation != expected) return;
    if (!candidate && currentWeb) {
      SHANDLE_PTR handle = 0;
      if (SUCCEEDED(currentWeb->get_HWND(&handle))) candidate = reinterpret_cast<HWND>(handle);
    }
    if (generation != expected || !candidate) return;
    candidate = GetAncestor(candidate, GA_ROOT);
    if (isFolderFrame(candidate) && !IsWindowVisible(candidate)) tabs::gate::arm(candidate, &generation, expected);
  }

  bool updateView() {
    const auto expected = generation;
    auto currentSite = site;
    auto currentWeb = web;
    if (!currentSite)
      return true;
    auto browser = shellBrowser(currentSite.Get());
    if (generation != expected)
      return false;
    if (!browser && currentWeb)
      browser = shellBrowser(currentWeb.Get());
    if (generation != expected || !browser)
      return false;

    HWND browserWindow = nullptr;
    browser->GetWindow(&browserWindow);
    if (generation != expected)
      return false;
    HWND candidate = browserWindow;
    if (currentWeb) {
      SHANDLE_PTR handle = 0;
      if (!candidate && SUCCEEDED(currentWeb->get_HWND(&handle)))
        candidate = reinterpret_cast<HWND>(handle);
    }
    if (generation != expected || !candidate)
      return false;
    candidate = GetAncestor(candidate, GA_ROOT);
    if (!isFolderFrame(candidate))
      return false;
    if (!tabsStopped && !IsWindowVisible(candidate)) {
      tabs::gate::arm(candidate, &generation, expected);
      if (generation != expected) return false;
    }
    if (navigation::tabsSupported() && !tabsFrame &&
        GetWindowThreadProcessId(candidate, nullptr) == ownerThread &&
        SetWindowSubclass(candidate, tabsFrameProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) tabsFrame = candidate;
    if (!IsWindowVisible(candidate))
      return false;
    bool tabsReady = true;
    if (navigation::tabsSupported() && !tabsStopped) {
      tabsReady = tabsHost.update(candidate, browser.Get(), currentWeb.Get());
      if (generation != expected) return false;
    }

    ComPtr<IShellView> active;
    HWND view = nullptr;
    HRESULT queried = browser->QueryActiveShellView(&active);
    if (generation != expected)
      return false;
    if (SUCCEEDED(queried) && active)
      active->GetWindow(&view);
    if (generation != expected)
      return false;
    HWND attachment = viewnav::attachmentWindow(candidate, browserWindow, view);
    if (!attachment)
      return false;

    if (host && host != attachment)
      detachController();
    if (generation != expected)
      return false;
    if (!controller) {
      // Win11 native tabs share a top-level frame but each has a browser site.
      // Deduplicate the tab host, never the entire Explorer window.
      HANDLE existing = GetPropW(attachment, ownerProperty);
      if (existing && existing != this)
        return view != nullptr && tabsReady;
      if (!SetPropW(attachment, ownerProperty, this))
        return false;
      host = attachment;
      // attach() queries Shell COM objects and can synchronously pump SetSite.
      // Publish only if this is still the site that requested the controller.
      auto *attached = viewnav::attach(attachment, browser.Get());
      if (generation != expected) {
        viewnav::detach(attached);
        return false;
      }
      controller = attached;
      if (!controller) {
        RemovePropW(host, ownerProperty);
        host = nullptr;
        return false;
      }
    } else {
      viewnav::refresh(controller, browser.Get());
    }
    if (generation != expected)
      return false;
    // A visible frame does not imply a ready Shell view. Keep the bounded
    // startup retries alive until the view is actually attached.
    return tabsReady && view && (navigation::configuredAction() == navigation::Action::Off ||
                    viewnav::hasView(controller));
  }

  static LRESULT CALLBACK helperProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                                     DWORD_PTR data) {
    auto *self = reinterpret_cast<BrowserHost *>(data);
    if (message == refreshMessage || message == quitMessage ||
        (message == WM_TIMER && wp == startupTimer)) {
      self->AddRef();
      const auto expected = self->generation;
      try {
        if (message == quitMessage) {
          self->disconnect();
        } else {
          if (message == refreshMessage)
            self->refreshPending = false;
          bool ready = self->updateView();
          if (self->generation == expected && self->helper == hwnd) {
            if (message == refreshMessage && !ready) {
              // A real Shell/window event starts another bounded attempt after
              // delayed creation. Never keep a background polling timer alive.
              self->startupAttempts = 0;
              SetTimer(hwnd, startupTimer, 250, nullptr);
            }
            if (message == WM_TIMER)
              ++self->startupAttempts;
            if (ready || self->startupAttempts >= 20)
              KillTimer(hwnd, startupTimer);
          }
        }
      } catch (...) {
        if (self->generation == expected && self->helper == hwnd) {
          KillTimer(hwnd, startupTimer);
          self->refreshPending = false;
        }
      }
      self->Release();
      return 0;
    }
    return DefSubclassProc(hwnd, message, wp, lp);
  }
  static LRESULT CALLBACK tabsFrameProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto *self = reinterpret_cast<BrowserHost *>(data); self->AddRef();
    bool syncHandled = false;
    try {
      static const UINT settingsChanged = RegisterWindowMessageW(L"ShibaKit.Navigation.SettingsChanged.v2");
      static const UINT syncTabs = RegisterWindowMessageW(L"ShibaKit.Navigation.SyncTabs.v1");
      static const UINT shutdownMsg = RegisterWindowMessageW(L"ShibaKit.Navigation.Shutdown.v2");
      if (message == settingsChanged) { self->tabsStopped = false; self->queueRefresh(); }
      if (message == syncTabs) { self->tabsStopped = false; self->updateView(); syncHandled = true; }
      if (message == shutdownMsg) { self->tabsStopped = true; self->tabsHost.clear(); }
      if (!self->tabsStopped && ((message == WM_SHOWWINDOW && w) ||
          (message == WM_ACTIVATE && LOWORD(w) != WA_INACTIVE) || message == WM_SIZE) &&
          !IsWindow(static_cast<HWND>(GetPropW(window, tabs::windowProperty)))) self->queueRefresh();
      if (message == WM_NCDESTROY) { self->tabsFrame = nullptr; RemoveWindowSubclass(window, tabsFrameProc, id); }
    } catch (...) { }
    // Several sites can share a frame. Always let the actual band owner's
    // subclass receive the synchronization message as well.
    auto result = DefSubclassProc(window, message, w, l);
    self->Release(); return syncHandled ? 1 : result;
  }

public:
  BrowserHost() { ++objects; }
  ~BrowserHost() {
    disconnect();
    --objects;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IObjectWithSite)
      return E_NOINTERFACE;
    *result = static_cast<IObjectWithSite *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void **result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    auto currentSite = site;
    return currentSite ? currentSite->QueryInterface(iid, result) : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetSite(IUnknown *value) override {
    if (GetCurrentThreadId() != ownerThread)
      return RPC_E_WRONG_THREAD;
    const auto expected = generation + 1;
    try {
      disconnect();
      if (generation != expected)
        return S_OK;
      if (!value)
        return S_OK;
      if (!isExplorerProcess())
        return E_ACCESSDENIED;
      ComPtr<IUnknown> newSite = value;
      auto newWeb = webBrowser(newSite.Get());
      if (generation != expected)
        return S_OK;
      // A context-menu site can provide IShellBrowser without IWebBrowser2.
      // The same host can attach there; automatic window loading uses the BHO site.
      auto browser = shellBrowser(newSite.Get());
      if (generation != expected)
        return S_OK;
      if (!browser && newWeb) browser = shellBrowser(newWeb.Get());
      if (generation != expected)
        return S_OK;
      if (!newWeb && !browser)
        return E_NOINTERFACE;
      gateHiddenFrame(browser.Get(), newWeb.Get(), expected);
      if (generation != expected)
        return S_OK;
      site = std::move(newSite);
      web = std::move(newWeb);
      helper = CreateWindowExW(0, L"STATIC", L"ShibaKit Navigation Host", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, module, nullptr);
      if (!helper) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        disconnect();
        return FAILED(hr) ? hr : E_FAIL;
      }
      if (!SetWindowSubclass(helper, helperProc, helperSubclass,
                             reinterpret_cast<DWORD_PTR>(this))) {
        disconnect();
        return E_FAIL;
      }
      connectEvents(expected);
      if (generation != expected)
        return S_OK;
      startupAttempts = 0;
      // SetSite can precede the visible view. Post the initial attach and make only
      // bounded, UI-thread retries during creation; no ongoing window polling.
      SetTimer(helper, startupTimer, 250, nullptr);
      queueRefresh();
      return S_OK;
    } catch (...) {
      if (generation == expected)
        disconnect();
      return E_FAIL;
    }
  }

  void queueRefresh() {
    if (helper && !refreshPending) {
      refreshPending = PostMessageW(helper, refreshMessage, 0, 0) != FALSE;
    }
  }
  void onEvent(DISPID id) {
    if (GetCurrentThreadId() != ownerThread)
      return;
    if (id == DISPID_NAVIGATECOMPLETE2 || id == DISPID_DOCUMENTCOMPLETE ||
        id == DISPID_WINDOWSTATECHANGED) {
      queueRefresh();
    } else if (id == DISPID_ONQUIT && helper) {
      PostMessageW(helper, quitMessage, 0, 0);
    }
  }
};

HRESULT BrowserEvents::Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *result,
                              EXCEPINFO *, UINT *) {
  if (result)
    VariantInit(result);
  BrowserHost *target = owner;
  if (target) {
    target->AddRef();
    target->onEvent(id);
    target->Release();
  }
  return S_OK;
}

class Factory final : public IClassFactory {
  std::atomic<ULONG> refs{1};
  bool tabBand = false;

public:
  explicit Factory(bool band = false) : tabBand(band) { ++objects; }
  ~Factory() { --objects; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IClassFactory)
      return E_NOINTERFACE;
    *result = static_cast<IClassFactory *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --refs;
    if (!count)
      delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid, void **result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (outer)
      return CLASS_E_NOAGGREGATION;
    if (tabBand) return tabs::create(iid, result);
    try {
      auto *instance = new BrowserHost();
      HRESULT hr = instance->QueryInterface(iid, result);
      instance->Release();
      return hr;
    } catch (...) { return E_OUTOFMEMORY; }
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    if (lock)
      ++serverLocks;
    else
      --serverLocks;
    return S_OK;
  }
};
void recordTabFactory(REFIID iid, HRESULT result) noexcept {
  constexpr auto key = L"Software\\ShibaKit";
  wchar_t text[40]{};
  if (StringFromGUID2(iid, text, ARRAYSIZE(text)))
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastFactoryInterface", REG_SZ, text, DWORD((wcslen(text) + 1) * sizeof(wchar_t)));
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastFactoryResult", REG_DWORD, &result, sizeof(result));
  DWORD process = GetCurrentProcessId();
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastFactoryProcess", REG_DWORD, &process, sizeof(process));
  HMODULE image = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&recordTabFactory), &image);
  std::wstring path(32768, L'\0');
  auto count = GetModuleFileNameW(image, path.data(), static_cast<DWORD>(path.size()));
  if (count && count < path.size())
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastFactoryModule", REG_SZ, path.c_str(), (count + 1) * sizeof(wchar_t));
}
} // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void **result) {
  const bool isTab = clsid == tabs::clsid;
  if (!result) {
    if (isTab) recordTabFactory(iid, E_POINTER);
    return E_POINTER;
  }
  *result = nullptr;
  if (clsid != navigationClsid && clsid != tabs::clsid)
    return CLASS_E_CLASSNOTAVAILABLE;
  if (isTab) recordTabFactory(iid, E_PENDING);
  auto *factory = new (std::nothrow) Factory(isTab);
  if (!factory) {
    if (isTab) recordTabFactory(iid, E_OUTOFMEMORY);
    return E_OUTOFMEMORY;
  }
  HRESULT hr = factory->QueryInterface(iid, result);
  factory->Release();
  if (isTab) recordTabFactory(iid, hr);
  return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
  return objects == 0 && serverLocks == 0 && viewnav::activeControllers() == 0 && tabs::activeObjects() == 0 &&
         tabs::gate::activeObjects() == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}
