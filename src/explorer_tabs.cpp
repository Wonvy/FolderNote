#include "language.h"
#include "explorer_tabs.h"
#include "tabs_model.h"
#include "tabs_routing.h"
#include "navigation.h"
#include "quick_icons.h"
#include "quick_folders.h"
#include "window_position.h"
#include <shlobj.h>
#include <shlguid.h>
#include <exdispid.h>
#include <ocidl.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <cstdlib>

namespace tabs {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t bandClass[] = L"FolderNote.ExplorerTabs.Band.v1";
constexpr UINT changedMessage = WM_APP + 121, actionMessage = WM_APP + 122, iconMessage = WM_APP + 123;
constexpr UINT failedMessage = WM_APP + 124;
constexpr UINT wheelMessage = WM_APP + 125;
std::atomic<long> objects{0}, bands{0};
enum Command : unsigned { New = 1, Duplicate, CopyPath, Close, CloseOthers, Restore, Settings, NewWindow, Select = 100 };
std::atomic<ULONG_PTR> transferSerial{static_cast<ULONG_PTR>(GetTickCount64())};
HMODULE codeModule() {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&codeModule), &module); return module;
}
bool explorerProcess() {
  std::wstring image(32768, L'\0');
  wchar_t system[MAX_PATH]{};
  GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
  GetWindowsDirectoryW(system, ARRAYSIZE(system));
  return !_wcsicmp(image.c_str(), (std::wstring(system) + L"\\explorer.exe").c_str());
}
template<class T> ComPtr<T> service(IUnknown *site, REFGUID id) {
  ComPtr<T> result; ComPtr<IServiceProvider> provider;
  if (site && SUCCEEDED(site->QueryInterface(IID_PPV_ARGS(&provider)))) {
    provider->QueryService(id, IID_PPV_ARGS(&result));
    // Some toolband sites expose the browser service through IUnknown only.
    if (!result) { ComPtr<IUnknown> identity; if (SUCCEEDED(provider->QueryService(id, IID_PPV_ARGS(&identity))) && identity) identity.As(&result); }
  }
  if (!result && site) site->QueryInterface(IID_PPV_ARGS(&result));
  return result;
}
void recordAttachment(const wchar_t *stage, HRESULT result) noexcept {
  try {
    static std::mutex mutex;
    static std::wstring lastStage;
    static HRESULT lastResult = E_PENDING;
    std::lock_guard lock(mutex);
    if (stage && lastStage == stage && lastResult == result) return;
    lastStage = stage ? stage : L"";
    lastResult = result;
  } catch (...) {}
  constexpr auto key = L"Software\\FolderNote";
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastBandStage", REG_SZ, stage, DWORD((wcslen(stage) + 1) * sizeof(wchar_t)));
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastBandResult", REG_DWORD, &result, sizeof(result));
  DWORD process = GetCurrentProcessId();
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastBandProcess", REG_DWORD, &process, sizeof(process));
  std::wstring path(32768, L'\0');
  DWORD size = GetModuleFileNameW(codeModule(), path.data(), static_cast<DWORD>(path.size()));
  if (size && size < path.size()) RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastBandModule", REG_SZ, path.c_str(), (size + 1) * sizeof(wchar_t));
}
Location location(PCIDLIST_ABSOLUTE id) {
  Location value; if (!id) return value;
  auto size = ILGetSize(id); const auto *bytes = reinterpret_cast<const BYTE *>(id); value.id.assign(bytes, bytes + size);
  PWSTR text = nullptr;
  if (SUCCEEDED(SHGetNameFromIDList(id, SIGDN_NORMALDISPLAY, &text))) { value.name = text; CoTaskMemFree(text); }
  if (SUCCEEDED(SHGetNameFromIDList(id, SIGDN_FILESYSPATH, &text))) { value.path = text; CoTaskMemFree(text); }
  if (value.name.empty()) value.name = lang::text(L"文件夹");
  return value;
}
Location current(IShellBrowser *browser) {
  ComPtr<IShellView> view; ComPtr<IFolderView> folder; ComPtr<IPersistFolder2> persistent;
  if (!browser || FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folder)) || !folder ||
      FAILED(folder->GetFolder(IID_PPV_ARGS(&persistent))) || !persistent) return {};
  PIDLIST_ABSOLUTE id = nullptr; Location value;
  if (SUCCEEDED(persistent->GetCurFolder(&id))) { value = location(id); CoTaskMemFree(id); }
  return value;
}
Location computer() {
  PIDLIST_ABSOLUTE id = nullptr; Location value;
  if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &id))) { value = location(id); CoTaskMemFree(id); }
  return value;
}
bool selection(IShellBrowser *browser, std::vector<std::vector<BYTE>> &items) {
  ComPtr<IShellView> view; ComPtr<IFolderView> folder; ComPtr<IEnumIDList> list;
  if (!browser || FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folder)) || !folder) return false;
  int count = 0;
  if (FAILED(folder->ItemCount(SVGIO_SELECTION, &count)) || count < 0 || size_t(count) > routing::maxSelection) return false;
  items.clear(); if (!count) return true;
  if (FAILED(folder->Items(SVGIO_SELECTION, IID_PPV_ARGS(&list))) || !list) return false;
  for (int index = 0; index < count; ++index) {
    PITEMID_CHILD id = nullptr;
    if (list->Next(1, &id, nullptr) != S_OK || !id) { CoTaskMemFree(id); return false; }
    const auto size = ILGetSize(id); auto bytes = reinterpret_cast<const BYTE *>(id);
    if (size > routing::maxPidlBytes) { CoTaskMemFree(id); return false; }
    items.emplace_back(bytes, bytes + size); CoTaskMemFree(id);
  }
  return true;
}
class Band;
class Events final : public IDispatch {
  std::atomic<ULONG> refs{1};
public:
  Band *owner;
  explicit Events(Band *value) : owner(value) { ++objects; }
  ~Events() { --objects; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDispatch && iid != DIID_DWebBrowserEvents2) return E_NOINTERFACE;
    *out = static_cast<IDispatch *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { auto count = --refs; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *count) override { if (!count) return E_POINTER; *count = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **out) override { if (out) *out = nullptr; return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return DISP_E_UNKNOWNNAME; }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *, UINT *) override;
};
class Band final : public IDeskBand2, public IObjectWithSite, public IPersistStream, public IInputObject {
  std::atomic<ULONG> refs{1};
  ComPtr<IUnknown> site;
  ComPtr<IShellBrowser> browser;
  ComPtr<IWebBrowser2> web;
  ComPtr<IInputObjectSite> input;
  ComPtr<IConnectionPoint> connection;
  ComPtr<Events> events;
  DWORD cookie = 0, ownerThread = GetCurrentThreadId(), bandId = 0;
  unsigned long long epoch = 0, viewRevision = 0, navigationRevision = 0;
  HWND window = nullptr, tabs = nullptr, overflow = nullptr, frame = nullptr;
  HWND blankSource = nullptr, suppressedSource = nullptr;
  POINT blankOrigin{}, lastBlankPoint{};
  Location blankFolder;
  size_t blankIndex = 0, blankCount = 0;
  unsigned long long blankEpoch = 0;
  DWORD lastBlankTime = 0;
  bool blankCancelled = false, lastBlankValid = false;
  HWND middleSource = nullptr;
  POINT middleOrigin{};
  Location middleFolder;
  size_t middleIndex = 0, middleCount = 0;
  unsigned long long middleEpoch = 0, middleRevision = 0, middleNavigation = 0;
  bool middleCancelled = false, wheelPosted = false;
  int wheelRemainder = 0, wheelTarget = -1;
  HFONT font = nullptr, symbols = nullptr;
  ui::Theme theme;
  std::unique_ptr<quick::icons::Cache> icons;
  quick::icons::Image folder;
  Model model;
  Model beforeNavigate;
  Location expected;
  size_t first = 0, beforeFirst = 0;
  std::vector<size_t> visible;
  std::vector<Location> displayed;
  std::wstring tipText;
  size_t displayedTotal = 0;
  UINT dpi = 96;
  int closeDown = -1, hot = -1, itemWidth = 0, itemHeight = 0;
  bool changing = false, waiting = false, composited = false;
  bool earlyNavigationError = false;
  ULONGLONG pendingDeadline = 0;
  struct Transfer {
    HWND peer = nullptr;
    routing::Packet packet;
    unsigned long long generation = 0, navigation = 0;
    ULONGLONG deadline = 0;
  } incoming, outgoing;
  bool importStarted = false;
  bool finishingImport = false, finishingExport = false;
  struct Finishing {
    bool &flag;
    explicit Finishing(bool &value) : flag(value) { flag = true; }
    ~Finishing() { flag = false; }
  };
  int px(int value) const { return MulDiv(value, dpi, 96); }
  struct Guard { Band *band; explicit Guard(Band *value):band(value){band->AddRef();} ~Guard(){band->Release();} };
  class TabDropTarget final : public IDropTarget {
    ULONG refs = 1;
    Band *band;
    ComPtr<IDataObject> source;
    ComPtr<IDropTarget> destination;
    std::vector<BYTE> destinationId;

    static ComPtr<IDropTarget> shellTarget(HWND frame, PCIDLIST_ABSOLUTE id) {
      ComPtr<IShellFolder> parent;
      PCUITEMID_CHILD child = nullptr;
      ComPtr<IDropTarget> target;
      if (!ILIsEmpty(id) && SUCCEEDED(SHBindToParent(id, IID_PPV_ARGS(&parent), &child)) && parent)
        parent->GetUIObjectOf(frame, 1, &child, IID_IDropTarget, nullptr,
                             reinterpret_cast<void **>(target.GetAddressOf()));
      if (!target && ILIsEmpty(id)) {
        ComPtr<IShellFolder> desktop;
        if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop)
          desktop->CreateViewObject(frame, IID_PPV_ARGS(&target));
      }
      return target;
    }

    int rowAt(POINTL point) const {
      return band && band->tabs && !band->changing && !band->waiting
          ? band->tabAt(nullptr, {point.x, point.y}) : -1;
    }

    const Location *locationAt(int row) const {
      if (!band || row < 0 || size_t(row) >= band->visible.size()) return nullptr;
      const auto index = band->visible[size_t(row)];
      return index < band->model.items.size() ? &band->model.items[index] : nullptr;
    }

    void leaveDestination() {
      auto previous = std::move(destination);
      destinationId.clear();
      if (band) band->paintDropRow(-1);
      if (previous) previous->DragLeave();
    }

    HRESULT update(DWORD keys, POINTL point, DWORD *effect) {
      const auto allowed = std::exchange(*effect, DWORD(DROPEFFECT_NONE));
      int row = rowAt(point);
      const auto *location = locationAt(row);
      if (!location || location->id.empty() || !source) { leaveDestination(); return S_OK; }
      auto id = location->id;
      DWORD resultEffect = allowed;
      HRESULT result = S_OK;
      if (destination && destinationId == id) {
        auto active = destination;
        result = active->DragOver(keys, point, &resultEffect);
      } else {
        leaveDestination();
        if (!band || !source) return S_OK;
        auto data = source;
        auto target = shellTarget(band->frame, reinterpret_cast<PCIDLIST_ABSOLUTE>(id.data()));
        if (!band || !source || !target) return S_OK;
        result = target->DragEnter(data.Get(), keys, point, &resultEffect);
        if (FAILED(result)) return S_OK;
        if (!band || !source) { target->DragLeave(); return S_OK; }
        destination = std::move(target);
        destinationId = std::move(id);
      }
      row = rowAt(point);
      location = locationAt(row);
      if (!location || destinationId != location->id || FAILED(result)) {
        leaveDestination();
        return S_OK;
      }
      *effect = resultEffect;
      band->paintDropRow(resultEffect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK) ? row : -1);
      return S_OK;
    }

  public:
    explicit TabDropTarget(Band *owner) : band(owner) { ++objects; }
    ~TabDropTarget() { --objects; }
    void disconnect() { band = nullptr; DragLeave(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
      if (!out) return E_POINTER;
      *out = nullptr;
      if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
      *out = static_cast<IDropTarget *>(this);
      AddRef();
      return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
      const auto count = --refs;
      if (!count) delete this;
      return count;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override {
      if (!effect) return E_POINTER;
      ComPtr<IDropTarget> keepAlive(this);
      DragLeave();
      if (!band) { *effect = DROPEFFECT_NONE; return S_OK; }
      source = data;
      return DragOver(keys, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD *effect) override {
      if (!effect) return E_POINTER;
      if (!band) { *effect = DROPEFFECT_NONE; return S_OK; }
      ComPtr<IDropTarget> keepAlive(this);
      Guard guard(band);
      try { return update(keys, point, effect); }
      catch (...) { *effect = DROPEFFECT_NONE; return S_OK; }
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
      ComPtr<IDropTarget> keepAlive(this);
      auto data = std::move(source);
      leaveDestination();
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override {
      if (!effect) return E_POINTER;
      ComPtr<IDropTarget> keepAlive(this);
      const auto *location = locationAt(rowAt(point));
      if (!location || !destination || location->id != destinationId || !data) {
        *effect = DROPEFFECT_NONE;
        return DragLeave();
      }
      Guard guard(band);
      auto target = std::move(destination);
      auto entered = std::move(source);
      destinationId.clear();
      band->paintDropRow(-1);
      const auto result = target->Drop(data, keys, point, effect);
      if (FAILED(result)) *effect = DROPEFFECT_NONE;
      return result;
    }
  };
  ComPtr<TabDropTarget> dropTarget;
  int dropRow = -1;
  void paintDropRow(int row) {
    if (std::exchange(dropRow, row) != row && tabs) InvalidateRect(tabs, nullptr, FALSE);
  }
  void stopDrop(HWND tabWindow) {
    auto target = std::move(dropTarget);
    dropRow = -1;
    if (!target) return;
    target->disconnect();
    RevokeDragDrop(tabWindow);
    OleUninitialize();
  }
  void startDrop() {
    const auto handle = tabs;
    if (FAILED(OleInitialize(nullptr))) return;
    ComPtr<TabDropTarget> target;
    target.Attach(new (std::nothrow) TabDropTarget(this));
    if (target && SUCCEEDED(RegisterDragDrop(handle, target.Get()))) {
      if (tabs == handle) {
        dropTarget = std::move(target);
        return;
      }
      target->disconnect();
      RevokeDragDrop(handle);
    }
    OleUninitialize();
  }
  void detach() {
    incoming = {}; outgoing = {}; importStarted = false;
    ++epoch; ++viewRevision; ++navigationRevision; changing = waiting = earlyNavigationError = false; pendingDeadline = 0;
    auto oldEvents = std::move(events); auto oldConnection = std::move(connection); auto oldCookie = std::exchange(cookie, 0);
    auto oldSite = std::move(site); auto oldBrowser = std::move(browser); auto oldWeb = std::move(web); auto oldInput = std::move(input);
    auto oldWindow = std::exchange(window, nullptr), oldTabs = std::exchange(tabs, nullptr), oldFrame = std::exchange(frame, nullptr);
    overflow = nullptr;
    blankSource = suppressedSource = middleSource = nullptr; blankFolder = {}; middleFolder = {}; lastBlankValid = false;
    wheelTarget = -1; wheelRemainder = 0; wheelPosted = false;
    auto oldFont = std::exchange(font, nullptr), oldSymbols = std::exchange(symbols, nullptr);
    if (oldEvents) oldEvents->owner = nullptr;
    icons.reset();
    model = {}; beforeNavigate = {}; expected = {}; visible.clear(); displayed.clear(); tipText.clear();
    displayedTotal = 0; itemWidth = itemHeight = 0;
    first = beforeFirst = 0; closeDown = hot = -1;
    // Clear all published state before window destruction or COM calls can reenter.
    if (oldFrame && GetPropW(oldFrame, windowProperty) == oldWindow) RemovePropW(oldFrame, windowProperty);
    stopDrop(oldTabs);
    if (oldTabs) RemoveWindowSubclass(oldTabs, tabProc, 1);
    if (oldWindow) {
      SetWindowLongPtrW(oldWindow, GWLP_USERDATA, 0);
      if (GetCapture() == oldWindow || (oldTabs && GetCapture() == oldTabs)) ReleaseCapture();
      DestroyWindow(oldWindow);
    }
    DeleteObject(oldFont); DeleteObject(oldSymbols);
    if (oldConnection && oldCookie) oldConnection->Unadvise(oldCookie);
  }
  void connectEvents(unsigned long long generation) {
    auto active = web; if (!active) return;
    ComPtr<IConnectionPointContainer> container; ComPtr<IConnectionPoint> point;
    if (FAILED(active.As(&container)) || epoch != generation ||
        FAILED(container->FindConnectionPoint(DIID_DWebBrowserEvents2, &point)) || epoch != generation) return;
    ComPtr<Events> sink; sink.Attach(new Events(this)); DWORD token = 0;
    auto result = point->Advise(sink.Get(), &token);
    if (FAILED(result) || epoch != generation) { sink->owner = nullptr; if (token) point->Unadvise(token); return; }
    connection = std::move(point); events = std::move(sink); cookie = token;
  }
  void fonts() {
    DeleteObject(font); DeleteObject(symbols); font = ui::makeFont(dpi, 12); symbols = ui::makeSymbolFont(dpi, 14);
    if (tabs) SendMessageW(tabs, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    icons = std::make_unique<quick::icons::Cache>(window, iconMessage, px(16));
    folder = quick::icons::read(quick::icons::Source::system(SIID_FOLDER), px(16));
    styleTooltip();
  }
  void styleTooltip() {
    if (!tabs) return;
    auto tooltip = TabCtrl_GetToolTips(tabs);
    if (!tooltip) return;
    SetWindowLongPtrW(tooltip, GWL_STYLE, GetWindowLongPtrW(tooltip, GWL_STYLE) | TTS_NOPREFIX | TTS_ALWAYSTIP);
    SetWindowTheme(tooltip, L"", L"");
    SendMessageW(tooltip, TTM_SETTIPBKCOLOR, theme.colors.surface, 0);
    SendMessageW(tooltip, TTM_SETTIPTEXTCOLOR, theme.colors.text, 0);
    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, px(640));
    SendMessageW(tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
  }
  bool position(HWND control, int x, int y, int width, int height) {
    RECT before{}; GetWindowRect(control, &before); MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&before), 2);
    RECT after{x, y, x + width, y + height};
    if (EqualRect(&before, &after)) return false;
    SetWindowPos(control, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    return true;
  }
  LRESULT route(HWND destination, HWND requester) {
    if (requester != routing::helperWindow() || !requester || !routing::enabled()) return 2;
    if (outgoing.peer) return 1;
    if (changing || waiting || incoming.peer || model.items.empty()) return 0;
    if (model.items.size() != 1 || destination == window || !routing::bandWindow(destination) ||
        GetAncestor(destination, GA_ROOT) == frame) return 2;
    const auto generation = epoch, revision = navigationRevision, viewVersion = viewRevision;
    auto active = browser; auto value = current(active.Get());
    if (epoch != generation || !window || revision != navigationRevision) return 2;
    if (changing || waiting || viewVersion != viewRevision) return 0;
    if (value.id.empty() || !(value == model.items[model.active])) return 0;
    routing::Packet packet; packet.token = ++transferSerial; packet.folder = value.id;
    const bool selected = selection(active.Get(), packet.selected);
    if (epoch != generation || revision != navigationRevision) return 2;
    if (!selected || viewVersion != viewRevision) return 0;
    // A tab's explicit "new window" command grants one short-lived exemption
    // for precisely this folder, instead of disabling global routing.
    if (routing::bypass(window, routing::bypassCheck, reinterpret_cast<PCIDLIST_ABSOLUTE>(packet.folder.data()))) return 2;
    if (epoch != generation || !window || revision != navigationRevision) return 2;
    if (viewVersion != viewRevision) return 0;
    auto bytes = routing::encode(packet); if (bytes.empty()) return 2;
    outgoing = {destination, packet, generation, revision, GetTickCount64() + 20000};
    SetTimer(window, 2, 20000, nullptr);
    COPYDATASTRUCT data{routing::importData, DWORD(bytes.size()), bytes.data()}; DWORD_PTR accepted = 0;
    // The receiver copies and queues the request. No COM navigation or closing
    // happens while either side is synchronously processing WM_COPYDATA.
    if (!SendMessageTimeoutW(destination, WM_COPYDATA, reinterpret_cast<WPARAM>(window), reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG, 500, &accepted) || !accepted) {
      if (epoch == generation) outgoing = {};
      return accepted == 2 ? 0 : 2;
    }
    if (accepted == 2) { if (epoch == generation) outgoing = {}; return 0; }
    return 1;
  }
  LRESULT receive(HWND source, const COPYDATASTRUCT *data) {
    if (!routing::enabled() || source == window || !routing::bandWindow(source) ||
        GetAncestor(source, GA_ROOT) == frame) return FALSE;
    if (changing || waiting || incoming.peer || outgoing.peer || model.items.empty()) return 2;
    routing::Packet packet; if (!routing::decode(data, packet)) return FALSE;
    const bool alreadyOpen = std::any_of(model.items.begin(), model.items.end(), [&](const Location &item) {
      return item.id == packet.folder;
    });
    if (model.items.size() >= Model::limit && !alreadyOpen) return FALSE;
    incoming = {source, std::move(packet), epoch, navigationRevision, GetTickCount64() + 20000};
    importStarted = false;
    if (!PostMessageW(window, routing::importMessage, 0, 0)) { incoming = {}; return FALSE; }
    return TRUE;
  }
  void beginImport() {
    if (!incoming.peer || importStarted) return;
    if (!routing::enabled() || incoming.generation != epoch || incoming.navigation != navigationRevision ||
        changing || waiting || !routing::bandWindow(incoming.peer)) { incoming = {}; return; }
    const auto request = incoming;
    auto destination = location(reinterpret_cast<PCIDLIST_ABSOLUTE>(request.packet.folder.data()));
    if (request.generation != epoch || request.navigation != navigationRevision ||
        incoming.packet.token != request.packet.token || !window || changing || waiting) return;
    auto existing = model.find(destination);
    if (!existing && !destination.id.empty()) {
      const auto snapshot = model.items;
      const auto activeIndex = model.active;
      const auto matches = [&](size_t index) {
        return !snapshot[index].id.empty() &&
            ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(snapshot[index].id.data()),
                      reinterpret_cast<PCIDLIST_ABSOLUTE>(destination.id.data()));
      };
      if (activeIndex < snapshot.size() && matches(activeIndex)) existing = activeIndex;
      else {
        for (size_t i = 0; i < snapshot.size(); ++i) {
          if (i != activeIndex && matches(i)) { existing = i; break; }
        }
      }
      if (request.generation != epoch || request.navigation != navigationRevision ||
          incoming.packet.token != request.packet.token || !window || changing || waiting) return;
      if (existing && (*existing >= model.items.size() || model.items[*existing].id != snapshot[*existing].id))
        existing = model.find(destination);
    }
    auto next = model;
    if (existing) {
      if (!next.select(*existing)) { incoming = {}; return; }
    } else if (!next.add(std::move(destination))) {
      incoming = {}; return;
    }
    importStarted = true;
    if (!go(std::move(next))) { incoming = {}; importStarted = false; }
    else if (incoming.peer == request.peer && incoming.packet.token == request.packet.token && epoch == request.generation)
      incoming.navigation = navigationRevision;
  }
  void finishImport(bool success) {
    if (!incoming.peer || !importStarted || finishingImport) return;
    Finishing finishing(finishingImport);
    const auto request = incoming;
    auto cancel = [&] {
      if (incoming.peer == request.peer && incoming.packet.token == request.packet.token) {
        incoming = {}; importStarted = false; if (window) KillTimer(window, 3);
      }
    };
    auto retry = [&] {
      if (window && request.generation == epoch && incoming.peer == request.peer && incoming.packet.token == request.packet.token)
        SetTimer(window, 3, 100, nullptr);
    };
    if (!success || request.generation != epoch || !window || !routing::enabled() ||
        GetTickCount64() > request.deadline || request.navigation != navigationRevision ||
        !routing::bandWindow(request.peer)) { cancel(); return; }
    const auto generation = epoch, revision = navigationRevision, viewVersion = viewRevision;
    auto active = browser; ComPtr<IShellView> view;
    const auto value = current(active.Get());
    if (epoch != generation || revision != navigationRevision) { cancel(); return; }
    if (value.id.empty() || viewVersion != viewRevision) { retry(); return; }
    if (!ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(value.id.data()), reinterpret_cast<PCIDLIST_ABSOLUTE>(request.packet.folder.data()))) {
      cancel(); return;
    }
    if (!active || FAILED(active->QueryActiveShellView(&view)) || !view || viewVersion != viewRevision) { retry(); return; }
    if (epoch != generation || revision != navigationRevision) { cancel(); return; }
    if (request.packet.selected.empty()) {
      // Activating the current folder tab skips BrowseObject. Clear that
      // existing view's selection to match the newly opened window.
      std::vector<std::vector<BYTE>> selected;
      if (!selection(active.Get(), selected) || !selected.empty()) {
        if (FAILED(view->SelectItem(nullptr, SVSI_DESELECTOTHERS))) { retry(); return; }
      }
    } else {
      for (size_t i = 0; i < request.packet.selected.size(); ++i) {
        auto id = reinterpret_cast<PCUITEMID_CHILD>(request.packet.selected[i].data());
        const auto flags = SVSI_SELECT | (i == 0 ? SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED : 0);
        if (FAILED(view->SelectItem(id, SVSIF(flags)))) { retry(); return; }
        if (epoch != generation || revision != navigationRevision) { cancel(); return; }
        if (viewVersion != viewRevision) { retry(); return; }
      }
    }
    if (epoch != generation || revision != navigationRevision) { cancel(); return; }
    if (viewVersion != viewRevision) { retry(); return; }
    std::vector<std::vector<BYTE>> restored;
    if (!selection(active.Get(), restored) || restored != request.packet.selected || viewVersion != viewRevision) { retry(); return; }
    if (epoch != generation || revision != navigationRevision) { cancel(); return; }
    if (!PostMessageW(request.peer, routing::completedMessage, reinterpret_cast<WPARAM>(window), static_cast<LPARAM>(request.packet.token))) {
      retry(); return;
    }
    cancel();
  }
  void completeExport(HWND destination, ULONG_PTR token) {
    if (!outgoing.peer || outgoing.peer != destination || outgoing.packet.token != token || finishingExport) return;
    Finishing finishing(finishingExport);
    const auto request = outgoing;
    auto cancel = [&] {
      if (outgoing.peer == request.peer && outgoing.packet.token == request.packet.token) {
        outgoing = {}; if (window) KillTimer(window, 4);
      }
    };
    auto retry = [&] {
      if (window && request.generation == epoch && outgoing.peer == request.peer && outgoing.packet.token == request.packet.token)
        SetTimer(window, 4, 100, nullptr);
    };
    if (!routing::enabled() || request.generation != epoch || request.navigation != navigationRevision ||
        GetTickCount64() > request.deadline || changing || waiting || model.items.size() != 1 ||
        !routing::bandWindow(destination)) { cancel(); return; }
    const auto viewVersion = viewRevision;
    auto active = browser; auto value = current(active.Get());
    if (request.generation != epoch || request.navigation != navigationRevision || !outgoing.peer) { cancel(); return; }
    if (value.id.empty() || viewVersion != viewRevision) { retry(); return; }
    if (!ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(value.id.data()),
        reinterpret_cast<PCIDLIST_ABSOLUTE>(request.packet.folder.data()))) { cancel(); return; }
    std::vector<std::vector<BYTE>> selected;
    if (!selection(active.Get(), selected) || viewVersion != viewRevision) { retry(); return; }
    if (selected != request.packet.selected || request.generation != epoch || request.navigation != navigationRevision ||
        !window || changing || waiting || model.items.size() != 1 || !(model.items[0] == value) ||
        !routing::bandWindow(destination) || !outgoing.peer) { cancel(); return; }
    auto targetFrame = GetAncestor(destination, GA_ROOT), sourceFrame = frame;
    const auto foreground = GetForegroundWindow();
    const bool activate = foreground == sourceFrame ||
        (GetPropW(sourceFrame, routing::gateProperty) && foreground &&
         GetPropW(sourceFrame, routing::gateOriginProperty) == foreground);
    cancel();
    // Execute on the source UI thread immediately after the final validation.
    // Keep this Band alive via proc's Guard while Explorer disconnects its site.
    SendMessageW(sourceFrame, WM_CLOSE, 0, 0);
    if (activate && IsWindow(targetFrame)) {
      if (IsIconic(targetFrame)) ShowWindowAsync(targetFrame, SW_RESTORE);
      SetForegroundWindow(targetFrame);
    }
  }
  void newWindow(size_t index) {
    const auto destination = model.items[index];
    auto id = reinterpret_cast<PCIDLIST_ABSOLUTE>(destination.id.data());
    const auto sender = window;
    const auto exemption = routing::enabled() ? routing::bypass(sender, routing::bypassAdd, id) : 0;
    if (routing::enabled() && routing::helperWindow() && !exemption) {
      ui::message(frame, lang::text(L"无法创建独立窗口，请稍后重试。"), L"FolderNote", MB_OK | MB_ICONERROR); return;
    }
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.hwnd = frame; launch.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"opennewwindow"; launch.lpIDList = const_cast<ITEMIDLIST *>(id); launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
      if (exemption) routing::bypass(sender, routing::bypassCancel, nullptr, exemption);
      ui::message(frame, lang::text(L"无法在新窗口打开此文件夹。"), L"FolderNote", MB_OK | MB_ICONERROR);
    }
  }
  void layout(bool repaint = false) {
    if (!window || !tabs) return;
    // Reserve a clickable blank area even when the visible tab row is full.
    RECT rect{}; GetClientRect(window, &rect); const int available = std::max(1, int(rect.right) - px(68));
    size_t count = std::max<size_t>(1, available / px(140)); count = std::min(count, model.items.size());
    if (!count) { visible.clear(); return; }
    if (model.active < first) first = model.active;
    if (model.active >= first + count) first = model.active - count + 1;
    first = std::min(first, model.items.size() - count);
    bool dirty = repaint || displayedTotal != model.items.size();
    std::vector<size_t> rows;
    std::vector<Location> values;
    for (size_t i = first; i < first + count; ++i) { rows.push_back(i); values.push_back(model.items[i]); }
    if (visible != rows) { hot = closeDown = -1; dirty = true; }
    visible = std::move(rows);
    // Keep native tab items alive. Shell sends several completion/size events
    // for one navigation; unchanged events must not hide or rebuild the strip.
    int previousCount = TabCtrl_GetItemCount(tabs);
    for (int row = previousCount; row > int(count); --row) { TabCtrl_DeleteItem(tabs, row - 1); dirty = true; }
    for (size_t row = 0; row < count; ++row) {
      TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = values[row].name.data();
      if (int(row) >= previousCount) { TabCtrl_InsertItem(tabs, int(row), &item); dirty = true; }
      else if (row >= displayed.size() || displayed[row].name != values[row].name) {
        TabCtrl_SetItem(tabs, int(row), &item); dirty = true;
      }
      if (row >= displayed.size() || displayed[row].id != values[row].id || displayed[row].path != values[row].path) dirty = true;
    }
    const int desiredWidth = std::min(px(190), std::max(px(70), (available - px(5)) / int(count)));
    const int desiredHeight = px(29);
    if (itemWidth != desiredWidth || itemHeight != desiredHeight) {
      TabCtrl_SetItemSize(tabs, desiredWidth, desiredHeight);
      itemWidth = desiredWidth; itemHeight = desiredHeight; dirty = true;
    }
    const int selected = int(model.active - first);
    if (TabCtrl_GetCurSel(tabs) != selected) { TabCtrl_SetCurSel(tabs, selected); dirty = true; }
    const int width = std::min(available, int(count) * itemWidth + px(5));
    bool moved = position(tabs, 0, 0, width, rect.bottom);
    moved = position(overflow, rect.right - px(32), px(2), px(30), px(28)) || moved;
    displayed = std::move(values); displayedTotal = model.items.size();
    if (dirty) if (auto tooltip = TabCtrl_GetToolTips(tabs)) SendMessageW(tooltip, TTM_POP, 0, 0);
    if (dirty || moved) InvalidateRect(tabs, nullptr, FALSE);
    if (repaint || moved) InvalidateRect(window, nullptr, FALSE);
    if (repaint) InvalidateRect(overflow, nullptr, FALSE);
  }
  void refresh() {
    if (changing || !browser || !window) return;
    const auto generation = epoch, revision = ++viewRevision;
    auto active = browser; auto value = current(active.Get());
    // QueryActiveShellView/GetCurFolder can pump a newer refresh without
    // changing the browser site. An older result must not overwrite it.
    if (generation != epoch || revision != viewRevision || !window || value.id.empty()) return;
    // Shell may complete a previous navigation while a newer one is loading.
    // Do not overwrite the newly selected tab with that previous location.
    if (waiting && !(value == expected)) return;
    waiting = false; pendingDeadline = 0; KillTimer(window, 1); beforeNavigate = {}; beforeFirst = 0;
    model.remember(std::move(value));
    const auto mode = ui::configuredTheme();
    const bool themeChanged = mode != theme.mode || !(ui::palette(mode) == theme.colors);
    if (themeChanged) { theme.reset(mode); styleTooltip(); }
    layout(themeChanged); finishImport(true); queueWheel();
  }
  void resolvePending() {
    if (!waiting || !browser || !window) return;
    const auto generation = epoch, revision = ++viewRevision, transaction = navigationRevision;
    auto active = browser; auto value = current(active.Get());
    // A pumped message can finish this navigation and start another one.
    // The old timeout must not shorten the new transaction's deadline.
    if (generation != epoch || transaction != navigationRevision || !window || !waiting) return;
    if (revision != viewRevision) { pendingDeadline = GetTickCount64() + 250; SetTimer(window, 1, 250, nullptr); return; }
    KillTimer(window, 1); waiting = false; pendingDeadline = 0;
    // A slow provider may never send a completion/error event. Keep the
    // destination tab only if its folder is actually displayed; otherwise
    // restore the previous selection before remembering the current view.
    // Query once so a reentrant navigation cannot overwrite the other tab.
    const bool success = !value.id.empty() && value == expected;
    if (!success) { model = std::move(beforeNavigate); first = beforeFirst; clearWheel(); }
    beforeNavigate = {}; expected = {}; beforeFirst = 0;
    model.remember(std::move(value)); layout(); finishImport(success); queueWheel();
  }
  bool go(Model next) {
    if (!browser || changing || next.items.empty()) return false;
    if (waiting) { layout(); return false; }
    const auto generation = epoch; auto active = browser;
    bool different = model.items.empty() || !(next.items[next.active] == model.items[model.active]);
    auto previous = model; const auto previousFirst = first;
    // Keep the target PIDL alive across COM reentrancy, including SetSite(nullptr).
    const auto destination = next.items[next.active];
    changing = true; earlyNavigationError = false; ++viewRevision; ++navigationRevision;
    HRESULT result = E_FAIL;
    try {
      model = std::move(next); layout();
      if (generation != epoch || !window) return false;
      // BrowseObject can pump size, paint and navigation messages. Publish the
      // selected target first so none of those messages paints the previous tab.
      result = different ? active->BrowseObject(reinterpret_cast<PCIDLIST_ABSOLUTE>(destination.id.data()), SBSP_ABSOLUTE | SBSP_SAMEBROWSER) : S_OK;
    } catch (...) {
      if (generation == epoch) {
        changing = false; model = std::move(previous); first = previousFirst; clearWheel();
        try { layout(); } catch (...) { }
      }
      return false;
    }
    if (generation != epoch || !window) return false;
    changing = false;
    if (FAILED(result)) { model = std::move(previous); first = previousFirst; clearWheel(); MessageBeep(MB_ICONWARNING); layout(); return false; }
    if (different) {
      beforeNavigate = std::move(previous); beforeFirst = previousFirst; expected = destination;
      waiting = true; pendingDeadline = GetTickCount64() + 15000; SetTimer(window, 1, 15000, nullptr);
      if (earlyNavigationError) PostMessageW(window, failedMessage, static_cast<WPARAM>(navigationRevision), 0);
    }
    PostMessageW(window, changedMessage, 0, 0); queueWheel(); return true;
  }
  void run(unsigned command, size_t index) {
    if (command == Settings) {
      try {
        // Resolve the current installation, even from an older loaded DLL.
        const auto executable = quick::executable();
        SHELLEXECUTEINFOW launch{sizeof(launch)};
        launch.fMask = SEE_MASK_FLAG_NO_UI; launch.hwnd = frame;
        launch.lpVerb = L"open"; launch.lpFile = executable.c_str();
        launch.lpParameters = L"--settings"; launch.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&launch)) return;
      } catch (...) { }
      ui::message(frame, lang::text(L"无法打开 FolderNote 设置，请从开始菜单重试；若程序文件已被移除，请重新安装。"), L"FolderNote", MB_OK | MB_ICONERROR);
      return;
    }
    if (changing || model.items.empty() || !navigation::tabsEnabled()) return;
    if (command != CopyPath) outgoing = {};
    if (command != CopyPath) clearWheel();
    auto next = model;
    if (command == New) { if (next.add(computer())) go(std::move(next)); }
    else if (command == Restore) { if (next.restore()) go(std::move(next)); }
    else if (index < model.items.size()) {
      if (command == NewWindow) newWindow(index);
      else if (command == Duplicate) { next.select(index); if (next.add(next.items[index])) go(std::move(next)); }
      else if (command == Close) { if (next.close(index)) go(std::move(next)); }
      else if (command == CloseOthers) { if (next.closeOthers(index)) go(std::move(next)); }
      else if (command == Select) { if (next.select(index)) go(std::move(next)); }
      else if (command == CopyPath && !model.items[index].path.empty()) {
        auto &path = model.items[index].path;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (path.size() + 1) * sizeof(wchar_t));
        if (!memory) return;
        void *data = GlobalLock(memory); if (!data) { GlobalFree(memory); return; }
        memcpy(data, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); GlobalUnlock(memory);
        if (!OpenClipboard(window)) { GlobalFree(memory); return; }
        if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
      }
    }
  }
  void menu(POINT point, int selected, bool all = false) {
    HMENU menu = CreatePopupMenu(); if (!menu) return;
    if (all) {
      for (size_t i = 0; i < model.items.size(); ++i) AppendMenuW(menu, MF_STRING | (i == model.active ? MF_CHECKED : 0), Select + i, model.items[i].name.c_str());
    } else {
      AppendMenuW(menu, MF_STRING | (model.items.size() >= Model::limit ? MF_GRAYED : 0), New, lang::text(L"新建标签"));
      bool valid = selected >= 0 && size_t(selected) < model.items.size();
      AppendMenuW(menu, MF_STRING | (!valid || model.items.size() >= Model::limit ? MF_GRAYED : 0), Duplicate, lang::text(L"复制标签"));
      AppendMenuW(menu, MF_STRING | (valid ? 0 : MF_GRAYED), NewWindow, lang::text(L"在新窗口打开"));
      AppendMenuW(menu, MF_STRING | (!valid || model.items[selected].path.empty() ? MF_GRAYED : 0), CopyPath, lang::text(L"复制文件夹路径"));
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      bool closable = valid && model.items.size() > 1;
      AppendMenuW(menu, MF_STRING | (closable ? 0 : MF_GRAYED), Close, lang::text(L"关闭标签"));
      AppendMenuW(menu, MF_STRING | (closable ? 0 : MF_GRAYED), CloseOthers, lang::text(L"关闭其他标签"));
      AppendMenuW(menu, MF_STRING | (model.closed.empty() ? MF_GRAYED : 0), Restore, lang::text(L"恢复刚关闭的标签"));
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      AppendMenuW(menu, MF_STRING, Settings, lang::text(L"设置"));
    }
    auto choice = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, window, nullptr); DestroyMenu(menu);
    if (choice) run(all ? Select : choice, all ? choice - Select : selected >= 0 ? size_t(selected) : model.active);
  }
  int hit(POINT point, bool close) const {
    for (size_t i = 0; i < visible.size(); ++i) {
      RECT rect{}; TabCtrl_GetItemRect(tabs, static_cast<int>(i), &rect);
      if (close) { rect.left = rect.right - px(23); rect.right -= px(3); }
      if (PtInRect(&rect, point)) return static_cast<int>(i);
    }
    return -1;
  }
  int tabAt(HWND source, POINT point) const {
    MapWindowPoints(source, tabs, &point, 1); return hit(point, false);
  }
  bool blankAt(HWND source, POINT point) const {
    MapWindowPoints(source, window, &point, 1);
    RECT bounds{}; GetClientRect(window, &bounds);
    if (!PtInRect(&bounds, point)) return false;
    RECT button{}; GetWindowRect(overflow, &button);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&button), 2);
    return !PtInRect(&button, point) && tabAt(window, point) < 0;
  }
  static bool modified(WPARAM keys) {
    return (keys & (MK_CONTROL | MK_SHIFT)) || (GetKeyState(VK_CONTROL) & 0x8000) ||
        (GetKeyState(VK_SHIFT) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
        (GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000);
  }
  void clearWheel() { wheelTarget = -1; wheelRemainder = 0; }
  void queueWheel() {
    if (wheelTarget < 0 || wheelPosted || changing || waiting || !window) return;
    wheelPosted = PostMessageW(window, wheelMessage, static_cast<WPARAM>(epoch), 0) != FALSE;
  }
  void drainWheel() {
    wheelPosted = false;
    if (changing || waiting || wheelTarget < 0 || !browser || !window) return;
    const auto target = std::exchange(wheelTarget, -1);
    if (!navigation::tabsEnabled()) { clearWheel(); return; }
    if (size_t(target) == model.active || size_t(target) >= model.items.size()) return;
    auto next = model;
    if (next.select(size_t(target))) go(std::move(next));
  }
  bool wheelInput(WPARAM keys, LPARAM position) {
    if (!tabs || model.items.empty() || !navigation::tabsEnabled()) return false;
    const auto generation = epoch;
    cancelMiddle();
    if (generation != epoch || !tabs) return false;
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
    ScreenToClient(tabs, &point);
    if (hit(point, false) < 0) { wheelRemainder = 0; return false; }
    if (modified(GET_KEYSTATE_WPARAM(keys))) { wheelRemainder = 0; return false; }
    const int total = wheelRemainder + GET_WHEEL_DELTA_WPARAM(keys);
    const int steps = total / WHEEL_DELTA;
    wheelRemainder = total % WHEEL_DELTA;
    if (steps) {
      const int from = wheelTarget >= 0 ? wheelTarget : int(model.active);
      wheelTarget = std::clamp(from - steps, 0, int(model.items.size()) - 1);
      queueWheel();
    }
    return true;
  }
  void cancelMiddle() {
    auto source = std::exchange(middleSource, nullptr);
    middleFolder = {}; middleCancelled = false;
    if (source && GetCapture() == source) ReleaseCapture();
  }
  bool middleMouse(HWND source, UINT message, WPARAM keys, LPARAM position) {
    if (message == WM_CAPTURECHANGED && middleSource == source && reinterpret_cast<HWND>(position) != source) {
      middleSource = nullptr; middleFolder = {}; return false;
    }
    if (message == WM_CANCELMODE || message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
        message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MOUSEWHEEL) {
      cancelMiddle();
      return false;
    }
    if (message != WM_MBUTTONDOWN && message != WM_MBUTTONDBLCLK &&
        message != WM_MOUSEMOVE && message != WM_MBUTTONUP) return false;
    POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)}, screen = point;
    ClientToScreen(source, &screen);
    if (message == WM_MBUTTONDOWN || message == WM_MBUTTONDBLCLK) {
      const auto generation = epoch;
      cancelMiddle(); cancelBlank();
      if (epoch != generation || source != tabs) return true;
      const int row = hit(point, false);
      if (row < 0 || model.items.size() < 2 || changing || waiting || modified(keys) ||
          (keys & (MK_LBUTTON | MK_RBUTTON)) || !navigation::tabsEnabled()) return true;
      middleIndex = visible[size_t(row)]; middleCount = model.items.size();
      middleFolder = model.items[middleIndex]; middleOrigin = screen;
      middleEpoch = epoch; middleRevision = viewRevision; middleNavigation = navigationRevision;
      middleCancelled = false; middleSource = source;
      SetCapture(source);
      if (GetCapture() != source) cancelMiddle();
      return true;
    }
    if (middleSource != source) return false;
    if (std::abs(screen.x - middleOrigin.x) >= GetSystemMetrics(SM_CXDRAG) ||
        std::abs(screen.y - middleOrigin.y) >= GetSystemMetrics(SM_CYDRAG) || modified(keys) ||
        (keys & (MK_LBUTTON | MK_RBUTTON))) middleCancelled = true;
    if (message == WM_MOUSEMOVE) return false;
    const int row = hit(point, false);
    const auto index = middleIndex, count = middleCount;
    const auto generation = middleEpoch, revision = middleRevision, navigation = middleNavigation;
    const auto pressedFolder = middleFolder;
    const bool accepted = !middleCancelled && GetCapture() == source && row >= 0 &&
        visible[size_t(row)] == index && epoch == generation && viewRevision == revision &&
        navigationRevision == navigation && model.items.size() == count && index < model.items.size() &&
        model.items[index] == pressedFolder && !changing && !waiting;
    cancelMiddle(); // Capture changes can synchronously detach or navigate the host.
    if (accepted && epoch == generation && viewRevision == revision && navigationRevision == navigation &&
        source == tabs && model.items.size() == count && index < model.items.size() && model.items[index] == pressedFolder)
      run(Close, index);
    return true;
  }
  void cancelBlank() {
    auto source = std::exchange(blankSource, nullptr);
    blankFolder = {}; blankCancelled = false;
    if (source && GetCapture() == source) ReleaseCapture();
  }
  bool blankMouse(HWND source, UINT message, WPARAM keys, LPARAM value) {
    if (message == WM_CAPTURECHANGED && blankSource == source && reinterpret_cast<HWND>(value) != source) {
      blankSource = nullptr; blankFolder = {}; return false;
    }
    if (message == WM_CANCELMODE || (message == WM_KEYDOWN && keys == VK_ESCAPE)) {
      cancelBlank(); suppressedSource = nullptr; return false;
    }
    POINT point{GET_X_LPARAM(value), GET_Y_LPARAM(value)}, screen = point;
    if (message != WM_LBUTTONDOWN && message != WM_LBUTTONDBLCLK && message != WM_MOUSEMOVE && message != WM_LBUTTONUP) return false;
    ClientToScreen(source, &screen);
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
      cancelBlank(); suppressedSource = nullptr;
      // A duplicated tab can occupy the point that was blank a moment ago.
      // Consume the second half of that double click instead of accidentally
      // selecting or closing a tab that moved under the pointer.
      const bool covered = lastBlankValid && DWORD(GetTickCount() - lastBlankTime) <= GetDoubleClickTime() &&
          std::abs(screen.x - lastBlankPoint.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
          std::abs(screen.y - lastBlankPoint.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2 && tabAt(source, point) >= 0;
      lastBlankValid = false;
      if (covered) { suppressedSource = source; return true; }
      if (modified(keys) || changing || waiting || model.items.empty() || !navigation::tabsEnabled() || !blankAt(source, point)) return false;
      blankFolder = model.items[model.active]; blankIndex = model.active; blankCount = model.items.size();
      blankEpoch = epoch; blankOrigin = screen; blankCancelled = false; blankSource = source;
      SetCapture(source);
      if (GetCapture() != source) cancelBlank();
      return true;
    }
    if (message == WM_LBUTTONUP && suppressedSource == source) { suppressedSource = nullptr; return true; }
    if (blankSource != source) return false;
    const bool dragged = std::abs(screen.x - blankOrigin.x) >= GetSystemMetrics(SM_CXDRAG) ||
                         std::abs(screen.y - blankOrigin.y) >= GetSystemMetrics(SM_CYDRAG);
    if (dragged || modified(keys)) blankCancelled = true;
    if (message == WM_MOUSEMOVE) return true;
    const bool accepted = !blankCancelled && GetCapture() == source && blankAt(source, point) &&
        epoch == blankEpoch && model.active == blankIndex && model.items.size() == blankCount &&
        !model.items.empty() && model.items[model.active] == blankFolder;
    const auto previousEpoch = epoch; const auto previousCount = model.items.size();
    const auto previousIndex = model.active; auto previousFolder = blankFolder;
    cancelBlank(); // ReleaseCapture synchronously sends WM_CAPTURECHANGED.
    if (accepted && epoch == previousEpoch && model.items.size() == previousCount &&
        model.active == previousIndex && !model.items.empty() && model.items[model.active] == previousFolder) {
      run(Duplicate, model.active);
      if (epoch == previousEpoch && model.items.size() == previousCount + 1) {
        lastBlankPoint = screen; lastBlankTime = GetTickCount(); lastBlankValid = true;
      }
    }
    return true;
  }
  void paintTabs(HDC dc) {
    RECT rect{}; GetClientRect(tabs, &rect); FillRect(dc, &rect, theme.background); const auto &p = theme.colors;
    auto old = SelectObject(dc, font);
    for (size_t row = 0; row < visible.size(); ++row) {
      RECT bounds{}; TabCtrl_GetItemRect(tabs, static_cast<int>(row), &bounds); InflateRect(&bounds, -px(2), -px(2));
      auto index = visible[row]; const auto &item = model.items[index]; bool selected = index == model.active;
      if (dropRow == int(row)) ui::rounded(dc, bounds, p.hover, p.accent, px(3));
      else if (selected || hot == int(row)) ui::rounded(dc, bounds, selected ? p.surface : p.hover, selected ? p.border : p.hover, 0);
      if (selected) { RECT line{bounds.left + px(8), bounds.bottom - px(2), bounds.right - px(8), bounds.bottom}; ui::rounded(dc, line, p.accent, p.accent, 0); }
      auto image = !item.path.empty() && icons ? icons->get(quick::icons::Source::file(item.path)) : quick::icons::Image{};
      HICON icon = image ? image->handle : folder ? folder->handle : nullptr;
      if (icon) DrawIconEx(dc, bounds.left + px(7), bounds.top + (bounds.bottom - bounds.top - px(16)) / 2, icon, px(16), px(16), 0, nullptr, DI_NORMAL);
      RECT label = bounds; label.left += px(29); label.right -= px(24); SelectObject(dc, font); ui::text(dc, item.name, label, p.text);
      if (model.items.size() > 1 && hot == int(row)) { RECT cross = bounds; cross.left = cross.right - px(22); SelectObject(dc, symbols); ui::text(dc, L"\xE711", cross, p.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
    }
    SelectObject(dc, old);
  }
  static LRESULT CALLBACK tabProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto *self = reinterpret_cast<Band *>(data); Guard guard(self);
    try {
      if (self->middleMouse(h, message, w, l)) return 0;
      if (message == WM_MOUSEWHEEL && self->wheelInput(w, l)) return 0;
      if (self->blankMouse(h, message, w, l)) return 0;
      if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc = BeginPaint(h, &paint);
        RECT bounds{}; GetClientRect(h, &bounds);
        auto buffer = CreateCompatibleDC(dc);
        auto bitmap = CreateCompatibleBitmap(dc, std::max<LONG>(1, bounds.right), std::max<LONG>(1, bounds.bottom));
        if (buffer && bitmap) {
          auto old = SelectObject(buffer, bitmap);
          try { self->paintTabs(buffer); }
          catch (...) { FillRect(buffer, &bounds, self->theme.background); }
          BitBlt(dc, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
          SelectObject(buffer, old);
        } else {
          auto saved = SaveDC(dc);
          try { self->paintTabs(dc); }
          catch (...) { FillRect(dc, &bounds, self->theme.background); }
          if (saved) RestoreDC(dc, saved);
        }
        if (bitmap) DeleteObject(bitmap); if (buffer) DeleteDC(buffer);
        EndPaint(h, &paint); return 0;
      }
      if (message == WM_ERASEBKGND) return 1;
      if (message == WM_MOUSEMOVE) {
        int hot = self->hit({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, false);
        if (hot != self->hot) { self->hot = hot; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0}; TrackMouseEvent(&track);
      }
      if (message == WM_MOUSELEAVE) { self->hot = -1; self->wheelRemainder = 0; InvalidateRect(h, nullptr, FALSE); }
      if (message == WM_LBUTTONDOWN && self->model.items.size() > 1) {
        const int row = self->hit({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, true);
        self->closeDown = row == self->hot ? row : -1; if (self->closeDown >= 0) return 0;
      }
      if (message == WM_LBUTTONUP && self->closeDown >= 0) {
        int row = self->hit({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, true); auto down = std::exchange(self->closeDown, -1);
        if (row == down && row == self->hot && size_t(row) < self->visible.size()) PostMessageW(self->window, actionMessage, Close, self->visible[row]); return 0;
      }
      if (message == WM_CONTEXTMENU) {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}, local = point;
        if (point.x == -1 && point.y == -1) { RECT rect{}; GetWindowRect(h, &rect); point = {rect.left + self->px(30), rect.bottom}; self->menu(point, static_cast<int>(self->model.active)); }
        else { ScreenToClient(h, &local); int row = self->hit(local, false); self->menu(point, row < 0 ? -1 : static_cast<int>(self->visible[row])); }
        return 0;
      }
      if (message == WM_KEYDOWN) {
        MSG key{h, message, w, l};
        if (self->TranslateAcceleratorIO(&key) == S_OK) return 0;
      }
      if (message == WM_SETFOCUS || message == WM_KILLFOCUS) if (self->input) self->input->OnFocusChangeIS(static_cast<IDeskBand2 *>(self), message == WM_SETFOCUS);
      if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(h, tabProc, id);
        if (self->tabs == h) self->stopDrop(h);
      }
    } catch (...) { }
    auto result = DefSubclassProc(h, message, w, l);
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_KEYDOWN || message == WM_SETFOCUS || message == WM_KILLFOCUS)
      InvalidateRect(h, nullptr, FALSE);
    return result;
  }
  static LRESULT CALLBACK proc(HWND h, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    auto *self = reinterpret_cast<Band *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, message, w, l);
    Guard guard(self);
    try {
      if (message == routing::routeMessage) return self->route(reinterpret_cast<HWND>(w), reinterpret_cast<HWND>(l));
      if (message == WM_COPYDATA) return self->receive(reinterpret_cast<HWND>(w), reinterpret_cast<const COPYDATASTRUCT *>(l));
      if (message == routing::importMessage) { self->beginImport(); return 0; }
      if (message == routing::completedMessage) { self->completeExport(reinterpret_cast<HWND>(w), static_cast<ULONG_PTR>(l)); return 0; }
      if (message == routing::capabilityMessage) return 2;
      if (message == WM_TIMER && w == 2) {
        KillTimer(h, 2); KillTimer(h, 4); self->outgoing = {};
        if (self->frame) PostMessageW(self->frame, routing::releaseGateMessage(), 0, 0);
        return 0;
      }
      if (message == WM_TIMER && w == 3) { KillTimer(h, 3); self->finishImport(true); return 0; }
      if (message == WM_TIMER && w == 4) {
        KillTimer(h, 4); const auto request = self->outgoing;
        self->completeExport(request.peer, request.packet.token); return 0;
      }
      if (message == wheelInputMessage) return self->wheelInput(w, l) ? TRUE : FALSE;
      if (message == wheelMessage) {
        if (w == static_cast<WPARAM>(self->epoch)) self->drainWheel();
        return 0;
      }
      if (self->middleMouse(h, message, w, l)) return 0;
      if (message == WM_MOUSEWHEEL && self->wheelInput(w, l)) return 0;
      if (self->blankMouse(h, message, w, l)) return 0;
      if (message == changedMessage) { self->refresh(); return 0; }
      if (message == failedMessage) {
        if (w && w != static_cast<WPARAM>(self->navigationRevision)) return 0;
        if (self->changing) { self->earlyNavigationError = true; return 0; }
        KillTimer(h, 1); self->resolvePending(); return 0;
      }
      if (message == WM_TIMER && w == 1) {
        const auto now = GetTickCount64();
        if (self->waiting && self->pendingDeadline > now) {
          SetTimer(h, 1, static_cast<UINT>(std::min<ULONGLONG>(self->pendingDeadline - now, USER_TIMER_MAXIMUM)), nullptr);
          return 0;
        }
        KillTimer(h, 1);
        self->resolvePending();
        return 0;
      }
      if (message == iconMessage) { InvalidateRect(self->tabs, nullptr, FALSE); return 0; }
      if (message == actionMessage) { self->run(static_cast<unsigned>(w), size_t(l)); return 0; }
      if (message == WM_SIZE) { self->layout(); return 0; }
      if (message == WM_ERASEBKGND) return 1;
      if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; auto dc = BeginPaint(h, &paint); RECT rect{}; GetClientRect(h, &rect);
        FillRect(dc, &rect, self->theme.background); EndPaint(h, &paint); return 0;
      }
      if (message == WM_NOTIFY && reinterpret_cast<NMHDR *>(l)->code == TTN_GETDISPINFOW &&
          reinterpret_cast<NMHDR *>(l)->hwndFrom == TabCtrl_GetToolTips(self->tabs)) {
        auto *info = reinterpret_cast<NMTTDISPINFOW *>(l);
        const auto row = info->hdr.idFrom;
        self->tipText.clear();
        if (row < self->visible.size()) {
          const auto &folder = self->model.items[self->visible[row]];
          self->tipText = folder.path.empty() ? folder.name : folder.path;
        }
        // Keep the complete Unicode path alive, beyond the 80-character
        // inline buffer; request it afresh after each directory change.
        info->lpszText = self->tipText.data(); info->uFlags &= ~TTF_DI_SETITEM;
        return 0;
      }
      if (message == WM_NOTIFY && reinterpret_cast<NMHDR *>(l)->hwndFrom == self->tabs) {
        const auto code = reinterpret_cast<NMHDR *>(l)->code;
        if (code == TCN_SELCHANGING) return self->changing || self->waiting;
        if (code == TCN_SELCHANGE) {
          const int selected = TabCtrl_GetCurSel(self->tabs);
          if (selected >= 0 && size_t(selected) < self->visible.size()) {
            const auto target = self->visible[selected]; self->run(Select, target);
          }
          return 0;
        }
      }
      if (message == WM_COMMAND && HIWORD(w) == BN_CLICKED) {
        if (LOWORD(w) == 3) { RECT rect{}; GetWindowRect(self->overflow, &rect); self->menu({rect.left, rect.bottom}, -1, true); }
        return 0;
      }
      if (message == WM_CONTEXTMENU) { POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; if (point.x == -1) { RECT rect{}; GetWindowRect(h, &rect); point = {rect.left, rect.bottom}; } self->menu(point, -1); return 0; }
      if (message == WM_DRAWITEM) {
        auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
        if (item->CtlType == ODT_BUTTON) {
          auto &p = self->theme.colors; FillRect(item->hDC, &item->rcItem, self->theme.background);
          bool hot = (item->itemState & (ODS_SELECTED | ODS_FOCUS | ODS_HOTLIGHT)) != 0;
          RECT button = item->rcItem; InflateRect(&button, -self->px(1), -self->px(1));
          if (hot) ui::rounded(item->hDC, button, p.hover, p.hover, 0);
          const int x = (button.left + button.right) / 2, y = (button.top + button.bottom) / 2;
          const int arm = self->px(5);
          auto pen = CreatePen(PS_SOLID, std::max(1, self->px(2)), item->itemState & ODS_DISABLED ? p.muted : p.text);
          auto old = SelectObject(item->hDC, pen);
          MoveToEx(item->hDC, x - arm, y - arm / 2, nullptr); LineTo(item->hDC, x, y + arm / 2);
          LineTo(item->hDC, x + arm, y - arm / 2);
          SelectObject(item->hDC, old); DeleteObject(pen); return TRUE;
        }
      }
      if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED || message == WM_SYSCOLORCHANGE || message == ui::themeChangedMessage()) {
        self->theme.reset(ui::configuredTheme()); self->styleTooltip(); self->layout(true); return 0;
      }
      if (message == WM_DPICHANGED_AFTERPARENT) { self->dpi = GetDpiForWindow(h); self->fonts(); self->layout(true); return 0; }
      if (message == WM_NCDESTROY) {
        if (self->frame && GetPropW(self->frame, windowProperty) == h) RemovePropW(self->frame, windowProperty);
        self->window = self->tabs = self->overflow = nullptr;
        self->blankSource = self->suppressedSource = nullptr; SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      }
    } catch (...) { /* Never unwind through Explorer's window procedure. */ }
    return DefWindowProcW(h, message, w, l);
  }
public:
  Band() { ++objects; ++bands; }
  ~Band() { detach(); --objects; if (--bands == 0) UnregisterClassW(bandClass, codeModule()); }
  void event(DISPID id) {
    if (id == DISPID_BEFORENAVIGATE2) { ++viewRevision; outgoing = {}; }
    if (window && (id == DISPID_NAVIGATECOMPLETE2 || id == DISPID_DOCUMENTCOMPLETE)) {
      ++viewRevision; PostMessageW(window, changedMessage, 0, 0);
    }
    if (window && id == DISPID_NAVIGATEERROR) PostMessageW(window, failedMessage, static_cast<WPARAM>(navigationRevision), 0);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IOleWindow || iid == IID_IDockingWindow || iid == IID_IDeskBand || iid == IID_IDeskBand2) *out = static_cast<IDeskBand2 *>(this);
    else if (iid == IID_IObjectWithSite) *out = static_cast<IObjectWithSite *>(this);
    else if (iid == IID_IPersist || iid == IID_IPersistStream) *out = static_cast<IPersistStream *>(this);
    else if (iid == IID_IInputObject) *out = static_cast<IInputObject *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { auto count = --refs; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE SetSite(IUnknown *value) override {
    if (ownerThread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    Guard guard(this); const auto generation = epoch + 1; detach();
    if (epoch != generation) return E_ABORT;
    if (!value) return S_OK;
    const wchar_t *stage = L"eligibility";
    auto finish = [&](HRESULT result) { recordAttachment(stage, result); return result; };
    if (!navigation::tabsSupported() || !navigation::tabsEnabled() || !explorerProcess()) return finish(E_ACCESSDENIED);
    try {
      ComPtr<IUnknown> newSite = value;
      ComPtr<IOleWindow> parentSite; HWND parent = nullptr;
      stage = L"parent-window";
      HRESULT result = newSite.As(&parentSite);
      if (FAILED(result) || epoch != generation) return finish(FAILED(result) ? result : E_ABORT);
      result = parentSite->GetWindow(&parent);
      if (FAILED(result) || epoch != generation || !IsWindow(parent)) return finish(FAILED(result) ? result : E_ABORT);
      if (GetWindowThreadProcessId(parent, nullptr) != ownerThread) return finish(RPC_E_WRONG_THREAD);
      stage = L"explorer-frame";
      frame = GetAncestor(parent, GA_ROOT); wchar_t name[64]{}; GetClassNameW(frame, name, ARRAYSIZE(name));
      if (wcscmp(name, L"CabinetWClass") && wcscmp(name, L"ExploreWClass")) { frame = nullptr; return finish(E_ACCESSDENIED); }
      stage = L"browser-service";
      auto newBrowser = service<IShellBrowser>(newSite.Get(), SID_STopLevelBrowser);
      if (epoch != generation) return finish(E_ABORT);
      if (!newBrowser) newBrowser = service<IShellBrowser>(newSite.Get(), SID_SInPlaceBrowser);
      if (epoch != generation) return finish(E_ABORT);
      auto newWeb = service<IWebBrowser2>(newSite.Get(), IID_IWebBrowserApp);
      if (epoch != generation) return finish(E_ABORT);
      // The rebar's site is not the BHO's site: obtain the top-level Shell
      // browser via its web-browser service when it does not expose it itself.
      if (!newBrowser && newWeb) newBrowser = service<IShellBrowser>(newWeb.Get(), SID_STopLevelBrowser);
      if (epoch != generation) return finish(E_ABORT);
      if (!newBrowser && newWeb) newBrowser = service<IShellBrowser>(newWeb.Get(), SID_SInPlaceBrowser);
      if (epoch != generation) return finish(E_ABORT);
      if (!newBrowser) newBrowser = service<IShellBrowser>(newSite.Get(), IID_IShellBrowser);
      if (epoch != generation) return finish(E_ABORT);
      ComPtr<IInputObjectSite> newInput; newSite.As(&newInput);
      if (epoch != generation) return finish(E_ABORT);
      if (!newBrowser) { detach(); return finish(E_NOINTERFACE); }
      site = std::move(newSite); browser = std::move(newBrowser); web = std::move(newWeb); input = std::move(newInput);
      stage = L"create-band-window";
      WNDCLASSW cls{}; cls.hInstance = codeModule(); cls.lpfnWndProc = proc; cls.lpszClassName = bandClass; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); RegisterClassW(&cls);
      window = CreateWindowExW(WS_EX_CONTROLPARENT, bandClass, lang::text(L"FolderNote 文件夹标签"), WS_CHILD | WS_CLIPCHILDREN,
          0, 0, 400, 34, parent, nullptr, codeModule(), this);
      if (!window) { detach(); return finish(E_FAIL); }
      dpi = GetDpiForWindow(window);
      INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES}; InitCommonControlsEx(&controls);
      tabs = CreateWindowW(WC_TABCONTROLW, lang::text(L"文件夹标签"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH | TCS_FOCUSONBUTTONDOWN | TCS_OWNERDRAWFIXED | TCS_TOOLTIPS,
          0, 0, 1, 1, window, reinterpret_cast<HMENU>(1), codeModule(), nullptr);
      overflow = CreateWindowW(L"BUTTON", lang::text(L"所有标签"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 1, 1, window, reinterpret_cast<HMENU>(3), codeModule(), nullptr);
      if (!tabs || !overflow) { detach(); return finish(E_FAIL); }
      SetWindowSubclass(tabs, tabProc, 1, reinterpret_cast<DWORD_PTR>(this)); fonts();
      SetPropW(frame, windowProperty, window);
      stage = L"connect-events";
      connectEvents(generation); if (epoch != generation) return finish(E_ABORT);
      stage = L"read-current-folder";
      refresh(); if (epoch != generation) return finish(E_ABORT);
      if (model.items.empty()) model.remember(computer());
      layout();
      if (epoch != generation || !tabs) return finish(E_ABORT);
      startDrop();
      if (epoch != generation || !tabs) return finish(E_ABORT);
      stage = L"attached";
      return finish(S_OK);
    } catch (...) { if (epoch == generation) detach(); return finish(E_FAIL); }
  }
  HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void **out) override { if (!out) return E_POINTER; *out = nullptr; auto current = site; return current ? current->QueryInterface(iid, out) : E_FAIL; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *out) override { if (!out) return E_POINTER; *out = window; return window ? S_OK : E_FAIL; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShowDW(BOOL show) override {
    if (window) {
      const bool desired = show && navigation::tabsEnabled();
      if (bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != desired)
        ShowWindow(window, desired ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CloseDW(DWORD) override { return ShowDW(FALSE); }
  HRESULT STDMETHODCALLTYPE ResizeBorderDW(const RECT *, IUnknown *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetBandInfo(DWORD id, DWORD, DESKBANDINFO *info) override {
    if (!info) return E_POINTER; bandId = id;
    if (info->dwMask & DBIM_MINSIZE) info->ptMinSize = {px(200), px(34)};
    if (info->dwMask & DBIM_MAXSIZE) info->ptMaxSize = {-1, px(34)};
    if (info->dwMask & DBIM_INTEGRAL) info->ptIntegral = {1, 1};
    if (info->dwMask & DBIM_ACTUAL) info->ptActual = {px(500), px(34)};
    if (info->dwMask & DBIM_TITLE) info->dwMask &= ~DBIM_TITLE;
    if (info->dwMask & DBIM_MODEFLAGS) info->dwModeFlags = DBIMF_NORMAL;
    if (info->dwMask & DBIM_BKCOLOR) info->crBkgnd = theme.colors.background;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CanRenderComposited(BOOL *out) override { if (!out) return E_POINTER; *out = FALSE; return S_OK; }
  HRESULT STDMETHODCALLTYPE SetCompositionState(BOOL value) override { composited = value != FALSE; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetCompositionState(BOOL *out) override { if (!out) return E_POINTER; *out = composited; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID *out) override { if (!out) return E_POINTER; *out = clsid; return S_OK; }
  HRESULT STDMETHODCALLTYPE IsDirty() override { return S_FALSE; }
  HRESULT STDMETHODCALLTYPE Load(IStream *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Save(IStream *, BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetSizeMax(ULARGE_INTEGER *out) override { if (!out) return E_POINTER; out->QuadPart = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE UIActivateIO(BOOL activate, MSG *) override { if (activate && tabs) SetFocus(tabs); return S_OK; }
  HRESULT STDMETHODCALLTYPE HasFocusIO() override { return window && (GetFocus() == window || IsChild(window, GetFocus())) ? S_OK : S_FALSE; }
  HRESULT STDMETHODCALLTYPE TranslateAcceleratorIO(MSG *message) override {
    if (!message || message->message != WM_KEYDOWN || HasFocusIO() != S_OK) return S_FALSE;
    if (GetKeyState(VK_CONTROL) & 0x8000) {
      if (message->wParam == 'T') { PostMessageW(window, actionMessage, New, 0); return S_OK; }
      if (message->wParam == 'W') { PostMessageW(window, actionMessage, Close, model.active); return S_OK; }
    }
    return S_FALSE;
  }
};
HRESULT Events::Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *result, EXCEPINFO *, UINT *) {
  if (result) VariantInit(result); if (owner) { auto target = owner; target->AddRef(); target->event(id); target->Release(); } return S_OK;
}
}
namespace {
class HostSite final : public IOleWindow, public IServiceProvider, public IInputObjectSite {
  std::atomic<ULONG> refs{1};
  HWND parent;
  ComPtr<IShellBrowser> browser;
  ComPtr<IWebBrowser2> web;
public:
  HostSite(HWND value, IShellBrowser *shell, IWebBrowser2 *automation) : parent(value), browser(shell), web(automation) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IOleWindow) *out = static_cast<IOleWindow *>(this);
    else if (iid == IID_IServiceProvider) *out = static_cast<IServiceProvider *>(this);
    else if (iid == IID_IInputObjectSite) *out = static_cast<IInputObjectSite *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { auto count = --refs; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *out) override { if (!out) return E_POINTER; *out = parent; return IsWindow(parent) ? S_OK : E_FAIL; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryService(REFGUID id, REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (browser && (id == SID_STopLevelBrowser || id == SID_SInPlaceBrowser || id == IID_IShellBrowser)) return browser->QueryInterface(iid, out);
    if (web && id == IID_IWebBrowserApp) return web->QueryInterface(iid, out);
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnFocusChangeIS(IUnknown *, BOOL) override { return S_OK; }
};
bool trustedRebar(HWND window, HWND frame, DWORD thread) {
  DWORD process = 0; wchar_t name[64]{};
  return IsWindow(window) && GetWindowThreadProcessId(window, &process) == thread && process == GetCurrentProcessId() &&
      GetAncestor(window, GA_ROOT) == frame && GetClassNameW(window, name, ARRAYSIZE(name)) && !_wcsicmp(name, REBARCLASSNAMEW);
}
bool usableRebar(HWND window, HWND frame, DWORD thread) {
  RECT bounds{};
  return trustedRebar(window, frame, thread) && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) &&
      GetClientRect(window, &bounds) && bounds.right > bounds.left && bounds.bottom > bounds.top;
}
bool navigationRebar(HWND window, DWORD thread) {
  bool address = false, travel = false;
  for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
    DWORD process = 0; wchar_t name[64]{};
    if (GetWindowThreadProcessId(child, &process) != thread || process != GetCurrentProcessId() ||
        !GetClassNameW(child, name, ARRAYSIZE(name))) continue;
    address = address || !_wcsicmp(name, L"Address Band Root");
    travel = travel || !_wcsicmp(name, L"TravelBand");
  }
  return address && travel;
}
HWND findRebar(HWND frame, IShellBrowser *browser, DWORD thread) {
  HWND toolbar = nullptr;
  if (browser && SUCCEEDED(browser->GetControlWindow(FCW_TOOLBAR, &toolbar))) {
    for (HWND candidate = toolbar; candidate && candidate != frame; candidate = GetParent(candidate))
      if (usableRebar(candidate, frame, thread)) return candidate;
  }
  struct Search { HWND frame, found, navigation; DWORD thread; unsigned count, navigationCount; } search{frame, nullptr, nullptr, thread, 0, 0};
  EnumChildWindows(frame, [](HWND child, LPARAM value) -> BOOL {
    auto &search = *reinterpret_cast<Search *>(value);
    if (usableRebar(child, search.frame, search.thread)) {
      search.found = child; ++search.count;
      if (navigationRebar(child, search.thread)) { search.navigation = child; ++search.navigationCount; }
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  // Win10 also creates a zero-height legacy toolbar below ShellTabWindowClass.
  // Prefer the actual navigation row, without choosing arbitrary toolbar order.
  if (search.navigationCount == 1) return search.navigation;
  return search.count == 1 ? search.found : nullptr;
}
int childBand(HWND rebar, HWND child) {
  auto count = SendMessageW(rebar, RB_GETBANDCOUNT, 0, 0);
  for (LRESULT index = 0; index < count; ++index) {
    REBARBANDINFOW info{sizeof(info)}; info.fMask = RBBIM_CHILD;
    if (SendMessageW(rebar, RB_GETBANDINFOW, index, reinterpret_cast<LPARAM>(&info)) && info.hwndChild == child) return int(index);
  }
  return -1;
}
}
struct Host::State : std::enable_shared_from_this<Host::State> {
  DWORD thread = GetCurrentThreadId();
  HWND frame = nullptr, rebar = nullptr, child = nullptr;
  ComPtr<IDeskBand2> band;
  ComPtr<IObjectWithSite> attachment;
  ComPtr<HostSite> site;
  unsigned long long generation = 0;
  int rebarWidth = -1;
  UINT bandDpi = 0;
  bool adjusting = false;
  void clear() {
    ++generation;
    auto oldRebar = std::exchange(rebar, nullptr), oldChild = std::exchange(child, nullptr); frame = nullptr;
    rebarWidth = -1; bandDpi = 0;
    auto oldBand = std::move(band); auto oldAttachment = std::move(attachment); auto oldSite = std::move(site);
    if (oldRebar && IsWindow(oldRebar)) {
      RemoveWindowSubclass(oldRebar, rebarProc, reinterpret_cast<UINT_PTR>(this));
      auto index = childBand(oldRebar, oldChild);
      if (index >= 0) SendMessageW(oldRebar, RB_DELETEBAND, index, 0);
    }
    if (oldAttachment) oldAttachment->SetSite(nullptr);
  }
  void resize() {
    if (adjusting || !IsWindow(rebar) || !IsWindow(child)) return;
    auto target = rebar, targetChild = child; const auto expected = generation;
    auto index = childBand(target, targetChild); if (index < 0 || generation != expected) return;
    RECT bounds{}; GetClientRect(target, &bounds);
    const auto dpi = GetDpiForWindow(target);
    const bool scaleChanged = dpi != bandDpi, widthChanged = bounds.right != rebarWidth;
    if (!scaleChanged && !widthChanged) return;
    bandDpi = dpi; rebarWidth = bounds.right;
    adjusting = true;
    if (scaleChanged) {
      auto pixels = [dpi](int value) { return MulDiv(value, dpi, 96); };
      REBARBANDINFOW info{sizeof(info)}; info.fMask = RBBIM_CHILDSIZE;
      info.cxMinChild = pixels(200); info.cyMinChild = info.cyChild = info.cyMaxChild = pixels(34); info.cyIntegral = 1;
      SendMessageW(target, RB_SETBANDINFOW, index, reinterpret_cast<LPARAM>(&info));
    }
    // A rebar notification can detach/recreate this host or reorder other bands.
    // Never apply the old index to a newer host or an unrelated toolbar row.
    if (widthChanged && generation == expected && rebar == target && child == targetChild) {
      index = childBand(target, targetChild);
      if (index >= 0 && generation == expected) SendMessageW(target, RB_MAXIMIZEBAND, index, FALSE);
    }
    adjusting = false;
  }
  static LRESULT CALLBACK rebarProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR value) {
    auto *raw = reinterpret_cast<State *>(value); auto self = raw->shared_from_this();
    if (message == WM_NCDESTROY) { self->clear(); return DefSubclassProc(window, message, w, l); }
    auto result = DefSubclassProc(window, message, w, l);
    if (message == WM_DPICHANGED_AFTERPARENT || message == WM_SIZE) self->resize();
    return result;
  }
  bool update(HWND target, IShellBrowser *browser, IWebBrowser2 *web) {
    if (GetCurrentThreadId() != thread) return false;
    if (!navigation::tabsSupported() || !navigation::tabsEnabled()) { clear(); return true; }
    DWORD process = 0; wchar_t name[64]{};
    if (!browser || !IsWindow(target) || GetWindowThreadProcessId(target, &process) != thread || process != GetCurrentProcessId() ||
        !GetClassNameW(target, name, ARRAYSIZE(name)) || (wcscmp(name, L"CabinetWClass") && wcscmp(name, L"ExploreWClass"))) return false;
    if (frame == target && IsWindow(rebar) && IsWindow(child) && childBand(rebar, child) >= 0) {
      auto currentBand = band;
      if (attachment && site && currentBand) { PostMessageW(child, changedMessage, 0, 0); currentBand->ShowDW(TRUE); }
      return true;
    }
    auto expected = generation + 1; clear();
    if (generation != expected) return false;
    auto parent = findRebar(target, browser, thread);
    if (generation != expected) return false;
    if (!parent) { recordAttachment(L"waiting-for-rebar", HRESULT_FROM_WIN32(ERROR_NOT_READY)); return false; }
    auto existing = static_cast<HWND>(GetPropW(target, windowProperty));
    // A detaching host can publish its child until SetSite(nullptr) completes.
    // Do not claim readiness for that child after its row has been removed.
    if (IsWindow(existing)) return GetParent(existing) == parent && childBand(parent, existing) >= 0 &&
        (GetWindowLongPtrW(existing, GWL_STYLE) & WS_VISIBLE); // Never destroy or duplicate another host.
    ComPtr<IDeskBand2> newBand; ComPtr<IObjectWithSite> newAttachment; ComPtr<HostSite> newSite;
    HRESULT result = create(IID_PPV_ARGS(&newBand));
    if (FAILED(result) || FAILED(newBand.As(&newAttachment))) return false;
    newSite.Attach(new HostSite(parent, browser, web));
    result = newAttachment->SetSite(static_cast<IOleWindow *>(newSite.Get()));
    if (FAILED(result) || generation != expected) { newAttachment->SetSite(nullptr); return false; }
    HWND newChild = nullptr; newBand->GetWindow(&newChild);
    if (!IsWindow(newChild)) { newAttachment->SetSite(nullptr); return false; }
    frame = target; rebar = parent; child = newChild; band = newBand; attachment = newAttachment; site = newSite;
    if (!SetWindowSubclass(parent, rebarProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) { clear(); return false; }
    UINT id = 0x464e5400;
    while (SendMessageW(parent, RB_IDTOINDEX, id, 0) >= 0) ++id;
    RECT rect{}; GetClientRect(parent, &rect); auto dpi = GetDpiForWindow(parent);
    REBARBANDINFOW info{sizeof(info)}; info.fMask = RBBIM_CHILD | RBBIM_STYLE | RBBIM_ID | RBBIM_CHILDSIZE | RBBIM_SIZE;
    info.hwndChild = newChild; info.wID = id; info.fStyle = RBBS_BREAK | RBBS_NOGRIPPER;
    info.cxMinChild = MulDiv(200, dpi, 96); info.cyMinChild = info.cyChild = info.cyMaxChild = MulDiv(34, dpi, 96); info.cyIntegral = 1;
    info.cx = std::max<LONG>(rect.right, LONG(info.cxMinChild));
    bandDpi = dpi; // RB_INSERTBAND already supplies these child dimensions.
    auto inserted = SendMessageW(parent, RB_INSERTBANDW, WPARAM(-1), reinterpret_cast<LPARAM>(&info));
    if (generation != expected) return false;
    if (!inserted || childBand(parent, newChild) < 0) { clear(); recordAttachment(L"insert-rebar-band", E_FAIL); return false; }
    newBand->ShowDW(TRUE); if (generation != expected) return false;
    resize(); if (generation != expected) return false;
    recordAttachment(L"hosted-in-rebar", S_OK); return true;
  }
};
Host::Host() : state(std::make_shared<State>()) {}
Host::~Host() { clear(); }
bool Host::update(HWND frame, IShellBrowser *browser, IWebBrowser2 *web) { auto current = state; return current->update(frame, browser, web); }
void Host::clear() { auto current = state; if (current && current->thread == GetCurrentThreadId()) current->clear(); }
HRESULT create(REFIID iid, void **out) {
  if (!out) return E_POINTER; *out = nullptr;
  recordAttachment(L"create-instance", E_PENDING);
  try { auto *band = new Band(); auto result = band->QueryInterface(iid, out); band->Release(); recordAttachment(L"created-instance", result); return result; }
  catch (...) { return E_OUTOFMEMORY; }
}
long activeObjects() { return objects.load(); }
}
