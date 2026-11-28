#include "../src/explorer_tabs.h"
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>
// Match Explorer's v6 common controls when exercising real rebar messages.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// Exercise the production band against a hidden, disposable Shell host. Only
// this test's process check is replaced; no Explorer window is modified.
static DWORD testExplorerPath(HMODULE instance, LPWSTR value, DWORD capacity) {
  if (instance) return GetModuleFileNameW(instance, value, capacity);
  wchar_t directory[MAX_PATH]{}; GetWindowsDirectoryW(directory, ARRAYSIZE(directory));
  auto path = std::wstring(directory) + L"\\explorer.exe";
  if (path.size() >= capacity) return capacity;
  memcpy(value, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); return DWORD(path.size());
}
// Exercise production DPI arithmetic without changing the user's monitors.
static UINT simulatedDpi = 0;
static UINT testWindowDpi(HWND window) { return simulatedDpi ? simulatedDpi : GetDpiForWindow(window); }
static HWND observedVisibilityWindow = nullptr;
static unsigned visibilityRequests = 0;
static BOOL testShowWindow(HWND window, int command) {
  if (window == observedVisibilityWindow) ++visibilityRequests;
  return ShowWindow(window, command);
}
static int simulatedModifier = 0;
static ULONGLONG simulatedElapsed = 0;
static ULONGLONG testTickCount64() { return GetTickCount64() + simulatedElapsed; }
static HWND observedTimerWindow = nullptr;
static std::vector<UINT> timerDelays;
static UINT_PTR testSetTimer(HWND window, UINT_PTR id, UINT delay, TIMERPROC procedure) {
  if (window == observedTimerWindow && id == 1) timerDelays.push_back(delay);
  return SetTimer(window, id, delay, procedure);
}
static SHORT testKeyState(int key) {
  if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN || key == VK_RWIN)
    return key == simulatedModifier ? SHORT(-32768) : 0;
  return GetKeyState(key);
}
struct RecordedLaunch { DWORD mask = 0; std::wstring verb; std::vector<BYTE> folder; };
static std::vector<RecordedLaunch> recordedLaunches;
static BOOL testShellExecuteExW(SHELLEXECUTEINFOW *launch) {
  RecordedLaunch recorded; recorded.mask = launch->fMask;
  if (launch->lpVerb) recorded.verb = launch->lpVerb;
  if ((launch->fMask & SEE_MASK_IDLIST) && launch->lpIDList) {
    auto id = static_cast<PCIDLIST_ABSOLUTE>(launch->lpIDList);
    auto bytes = reinterpret_cast<const BYTE *>(id); recorded.folder.assign(bytes, bytes + ILGetSize(id));
  }
  recordedLaunches.push_back(std::move(recorded)); return TRUE;
}
static HWND testOwnedHelper = nullptr;
static HWND testFindWindowExW(HWND parent, HWND after, LPCWSTR className, LPCWSTR title) {
  if (parent == HWND_MESSAGE && reinterpret_cast<ULONG_PTR>(className) > 0xffff &&
      !wcscmp(className, L"ShibaKit.QuickFolders.Helper.v1")) return testOwnedHelper;
  return FindWindowExW(parent, after, className, title);
}
static HRESULT WINAPI testBindToParent(PCIDLIST_ABSOLUTE, REFIID, void **, PCUITEMID_CHILD *);
static Microsoft::WRL::ComPtr<IDropTarget> observedDropTarget;
static bool observeDropTarget = false;
static HRESULT WINAPI testRegisterDragDrop(HWND window, IDropTarget *target) {
  const auto result = RegisterDragDrop(window, target);
  if (SUCCEEDED(result) && observeDropTarget) observedDropTarget = target;
  return result;
}
#define GetModuleFileNameW testExplorerPath
#define GetDpiForWindow testWindowDpi
#define ShowWindow testShowWindow
#define GetKeyState testKeyState
#define GetAsyncKeyState testKeyState
#define SetTimer testSetTimer
#define GetTickCount64 testTickCount64
#define ShellExecuteExW testShellExecuteExW
#define FindWindowExW testFindWindowExW
#define SHBindToParent testBindToParent
#define RegisterDragDrop testRegisterDragDrop
#include "../src/explorer_tabs.cpp"
#undef RegisterDragDrop
#undef SHBindToParent
#undef FindWindowExW
#undef ShellExecuteExW
#undef GetTickCount64
#undef SetTimer
#undef GetAsyncKeyState
#undef GetKeyState
#undef ShowWindow
#undef GetDpiForWindow
#undef GetModuleFileNameW

using Microsoft::WRL::ComPtr;
static int checks = 0;
static void require(bool ok, const char *message) { ++checks; if (!ok) throw message; }
static void pump() { MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } }
static void expirePending(HWND window) { simulatedElapsed += 15001; SendMessageW(window, WM_TIMER, 1, 0); }

enum class DropEvent { Enter, Over, Leave, Drop };
struct DropCall {
  DropEvent event;
  IDataObject *data;
  DWORD keys;
  POINTL point;
  DWORD allowed;
};
struct RecordingDropTarget final : IDropTarget {
  ULONG refs = 1;
  std::vector<DropCall> calls;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
    *out = static_cast<IDropTarget *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT record(DropEvent event, IDataObject *data, DWORD keys, POINTL point, DWORD *effect) {
    calls.push_back({event, data, keys, point, *effect});
    const DWORD preferred = keys & MK_CONTROL ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    *effect &= preferred;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override {
    return record(DropEvent::Enter, data, keys, point, effect);
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL point, DWORD *effect) override {
    return record(DropEvent::Over, nullptr, keys, point, effect);
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override {
    calls.push_back({DropEvent::Leave, nullptr, 0, {}, 0});
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD keys, POINTL point, DWORD *effect) override {
    return record(DropEvent::Drop, data, keys, point, effect);
  }
};
struct RecordingDropParent final : IShellFolder {
  ULONG refs = 1;
  std::array<tabs::Location, 2> locations;
  std::array<RecordingDropTarget, 2> targets;
  RecordingDropTarget *bound = nullptr;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IShellFolder) return E_NOINTERFACE;
    *out = static_cast<IShellFolder *>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE ParseDisplayName(HWND, IBindCtx *, LPWSTR, ULONG *, PIDLIST_RELATIVE *, ULONG *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumObjects(HWND, SHCONTF, IEnumIDList **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BindToObject(PCUIDLIST_RELATIVE, IBindCtx *, REFIID, void **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BindToStorage(PCUIDLIST_RELATIVE, IBindCtx *, REFIID, void **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CompareIDs(LPARAM, PCUIDLIST_RELATIVE, PCUIDLIST_RELATIVE) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateViewObject(HWND, REFIID, void **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAttributesOf(UINT, PCUITEMID_CHILD_ARRAY, SFGAOF *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetUIObjectOf(HWND, UINT count, PCUITEMID_CHILD_ARRAY, REFIID iid, UINT *, void **out) override {
    return count == 1 && bound ? bound->QueryInterface(iid, out) : E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDisplayNameOf(PCUITEMID_CHILD, SHGDNF, STRRET *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetNameOf(HWND, PCUITEMID_CHILD, LPCWSTR, SHGDNF, PITEMID_CHILD *) override { return E_NOTIMPL; }
};
static RecordingDropParent *dropParent = nullptr;
static HRESULT WINAPI testBindToParent(PCIDLIST_ABSOLUTE id, REFIID iid, void **out, PCUITEMID_CHILD *child) {
  if (dropParent) {
    for (size_t i = 0; i < dropParent->locations.size(); ++i) {
      const auto &location = dropParent->locations[i];
      const auto *bytes = reinterpret_cast<const BYTE *>(id);
      if (location.id.size() != ILGetSize(id) || !std::equal(location.id.begin(), location.id.end(), bytes)) continue;
      dropParent->bound = &dropParent->targets[i];
      if (child) *child = ILFindLastID(id);
      return dropParent->QueryInterface(iid, out);
    }
  }
  return SHBindToParent(id, iid, out, child);
}
static tabs::Location known(REFKNOWNFOLDERID folder) {
  PIDLIST_ABSOLUTE id = nullptr;
  require(SUCCEEDED(SHGetKnownFolderIDList(folder, 0, nullptr, &id)), "known folder PIDL");
  auto value = tabs::location(id); CoTaskMemFree(id); return value;
}
static void models() {
  auto a = known(FOLDERID_Windows), b = known(FOLDERID_Profile), c = known(FOLDERID_ComputerFolder);
  tabs::Model model;
  require(!model.add({}) && !model.select(0) && !model.close(0), "invalid operations safe");
  model.remember(a); require(!model.close(0), "last tab remains open");
  require(model.add(model.items[0]), "duplicate from own vector survives reallocation");
  model.remember(b); require(model.items[0] == a && model.items[1] == b, "directory state per tab");
  model.add(c); model.select(1); model.close(0);
  require(model.active == 0 && model.items[0] == b, "closing before active preserves selection");
  model.close(0); require(model.items[0] == c, "closing active selects adjacent tab");
  require(model.restore() && model.items[model.active] == b, "restore latest closed folder");
  model.add(a); model.select(1); model.closeOthers(1);
  require(model.items.size() == 1 && model.items[0] == b && model.active == 0, "close others retains target");
  require(!model.find({}).has_value() && !model.find(c).has_value(), "find ignores empty or absent locations");
  model.add(a); model.add(b); model.add(a);
  require(model.items.size() == 4 && model.active == 3 && model.find(a) == size_t(3), "find prefers active duplicate tab");
  model.select(2);
  require(model.find(a) == size_t(1), "find returns first matching tab when inactive");
  tabs::Model separate; separate.remember(c);
  for (size_t i = model.items.size(); i < tabs::Model::limit; ++i) require(model.add(a), "add within bounded tab limit");
  require(!model.add(b) && !model.restore(), "limit preserves closed history");
  while (model.items.size() > 1) model.close(0);
  require(model.closed.size() == tabs::Model::closedLimit, "closed history bounded");
  require(separate.items.size() == 1 && separate.items[0] == c, "different windows isolated");
}
struct SelectedItems final : IEnumIDList {
  ULONG refs = 1; size_t at = 0;
  std::vector<std::vector<BYTE>> items;
  explicit SelectedItems(std::vector<std::vector<BYTE>> value) : items(std::move(value)) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IEnumIDList) return E_NOINTERFACE;
    *out = static_cast<IEnumIDList *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { auto count = --refs; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, PITEMID_CHILD *out, ULONG *fetched) override {
    if (!out || (count != 1 && !fetched)) return E_POINTER;
    ULONG copied = 0;
    while (copied < count && at < items.size()) {
      out[copied] = static_cast<PITEMID_CHILD>(CoTaskMemAlloc(items[at].size()));
      if (!out[copied]) return E_OUTOFMEMORY;
      memcpy(out[copied], items[at].data(), items[at].size()); ++copied; ++at;
    }
    if (fetched) *fetched = copied; return copied == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override { auto before = at; at = std::min(items.size(), at + count); return at - before == count ? S_OK : S_FALSE; }
  HRESULT STDMETHODCALLTYPE Reset() override { at = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE Clone(IEnumIDList **out) override {
    if (!out) return E_POINTER; auto copy = new SelectedItems(items); copy->at = at; *out = copy; return S_OK;
  }
};
struct View final : IShellView, IFolderView, IPersistFolder2 {
  ULONG refs = 1;
  tabs::Location folder = known(FOLDERID_Windows);
  std::function<void()> onGetFolder;
  std::function<void()> onSelectItem;
  std::vector<std::vector<BYTE>> selected;
  std::vector<SVSIF> selectionFlags;
  HRESULT selectOutcome = S_OK;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IShellView || iid == IID_IOleWindow) *out = static_cast<IShellView *>(this);
    else if (iid == IID_IFolderView) *out = static_cast<IFolderView *>(this);
    else if (iid == IID_IPersist || iid == IID_IPersistFolder || iid == IID_IPersistFolder2) *out = static_cast<IPersistFolder2 *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UIActivate(UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Refresh() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CreateViewWindow(IShellView *, LPCFOLDERSETTINGS, IShellBrowser *, RECT *, HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DestroyViewWindow() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurrentInfo(LPFOLDERSETTINGS) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE AddPropertySheetPages(DWORD, LPFNSVADDPROPSHEETPAGE, LPARAM) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SaveViewState() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SelectItem(PCUITEMID_CHILD id, SVSIF flags) override {
    if (onSelectItem) { auto callback = std::move(onSelectItem); callback(); }
    if (FAILED(selectOutcome)) return selectOutcome;
    if (flags & SVSI_DESELECTOTHERS) selected.clear();
    if (!id) {
      if (!(flags & SVSI_DESELECTOTHERS)) return E_INVALIDARG;
      selectionFlags.push_back(flags); return S_OK;
    }
    auto bytes = reinterpret_cast<const BYTE *>(id); selected.emplace_back(bytes, bytes + ILGetSize(id));
    selectionFlags.push_back(flags); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetItemObject(UINT, REFIID, void **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurrentViewMode(UINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetCurrentViewMode(UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFolder(REFIID iid, void **out) override { return QueryInterface(iid, out); }
  HRESULT STDMETHODCALLTYPE Item(int, PITEMID_CHILD *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ItemCount(UINT flags, int *out) override {
    if (!out) return E_POINTER; if (flags != SVGIO_SELECTION) return E_NOTIMPL;
    *out = static_cast<int>(selected.size()); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Items(UINT flags, REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr; if (flags != SVGIO_SELECTION) return E_NOTIMPL;
    auto list = new SelectedItems(selected); auto result = list->QueryInterface(iid, out); list->Release(); return result;
  }
  HRESULT STDMETHODCALLTYPE GetSelectionMarkedItem(int *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFocusedItem(int *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetItemPosition(PCUITEMID_CHILD, POINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSpacing(POINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDefaultSpacing(POINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAutoArrange() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SelectItem(int, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT *, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurFolder(PIDLIST_ABSOLUTE *out) override {
    if (!out) return E_POINTER;
    *out = ILCloneFull(reinterpret_cast<PCIDLIST_ABSOLUTE>(folder.id.data()));
    // A Shell provider may dispatch a newer completion while an older folder
    // query is still returning its already captured PIDL.
    if (onGetFolder) { auto callback = std::move(onGetFolder); callback(); }
    return *out ? S_OK : E_OUTOFMEMORY;
  }
};
struct Browser final : IShellBrowser {
  ULONG refs = 1;
  HWND parent = nullptr;
  View view;
  HRESULT outcome = S_OK;
  bool complete = true;
  int navigations = 0;
  int browseDepth = 0, maximumBrowseDepth = 0;
  std::function<void()> onBrowse, onWindow, onView;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IOleWindow && iid != IID_IShellBrowser) return E_NOINTERFACE;
    *out = static_cast<IShellBrowser *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *out) override { if (!out) return E_POINTER; *out = parent; if (onWindow) onWindow(); return S_OK; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE InsertMenusSB(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetMenuSB(HMENU, HOLEMENU, HWND) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RemoveMenusSB(HMENU) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetStatusTextSB(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableModelessSB(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TranslateAcceleratorSB(MSG *, WORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BrowseObject(PCUIDLIST_RELATIVE id, UINT flags) override {
    struct Depth { Browser *browser; explicit Depth(Browser *value) : browser(value) {
      ++browser->browseDepth; browser->maximumBrowseDepth = std::max(browser->maximumBrowseDepth, browser->browseDepth);
    } ~Depth() { --browser->browseDepth; } } depth(this);
    ++navigations; require((flags & SBSP_SAMEBROWSER) != 0, "navigation remains in current window");
    if (onBrowse) onBrowse();
    if (SUCCEEDED(outcome) && complete) view.folder = tabs::location(reinterpret_cast<PCIDLIST_ABSOLUTE>(id));
    return outcome;
  }
  HRESULT STDMETHODCALLTYPE GetViewStateStream(DWORD, IStream **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetControlWindow(UINT, HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SendControlMsg(UINT, UINT, WPARAM, LPARAM, LRESULT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView **out) override {
    if (!out) return E_POINTER; if (onView) onView(); *out = &view; view.AddRef(); return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnViewWindowActive(IShellView *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetToolbarItems(LPTBBUTTONSB, UINT, UINT) override { return E_NOTIMPL; }
};

// A real toolband is hosted by a rebar site, not directly by IShellBrowser.
// Expose only the IWebBrowserApp service here; its browser can reach Shell.
#pragma warning(push)
#pragma warning(disable: 4100)
struct WebService final : IWebBrowser2, IServiceProvider {
  ULONG refs = 1;
  Browser *browser;
  bool available = true;
  std::function<void()> onService;
  explicit WebService(Browser *value) : browser(value) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IWebBrowser || iid == IID_IWebBrowserApp || iid == IID_IWebBrowser2) *out = static_cast<IWebBrowser2 *>(this);
    else if (iid == IID_IServiceProvider) *out = static_cast<IServiceProvider *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE QueryService(REFGUID id, REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (onService) onService();
    return available && id == SID_STopLevelBrowser ? browser->QueryInterface(iid, out) : E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *, UINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GoBack( void) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GoForward( void) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GoHome( void) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GoSearch( void) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Navigate( BSTR URL, VARIANT *Flags, VARIANT *TargetFrameName, VARIANT *PostData, VARIANT *Headers) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Refresh( void) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Refresh2( VARIANT *Level) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Stop( void) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Application( IDispatch **ppDisp) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Parent( IDispatch **ppDisp) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Container( IDispatch **ppDisp) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Document( IDispatch **ppDisp) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_TopLevelContainer( VARIANT_BOOL *pBool) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Type( BSTR *Type) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Left( long *pl) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Left( long Left) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Top( long *pl) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Top( long Top) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Width( long *pl) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Width( long Width) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Height( long *pl) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Height( long Height) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_LocationName( BSTR *LocationName) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_LocationURL( BSTR *LocationURL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Busy( VARIANT_BOOL *pBool) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Quit( void) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ClientToWindow( int *pcx, int *pcy) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PutProperty( BSTR Property, VARIANT vtValue) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetProperty( BSTR Property, VARIANT *pvtValue) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Name( BSTR *Name) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_HWND( SHANDLE_PTR *pHWND) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_FullName( BSTR *FullName) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Path( BSTR *Path) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Visible( VARIANT_BOOL *pBool) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Visible( VARIANT_BOOL Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_StatusBar( VARIANT_BOOL *pBool) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_StatusBar( VARIANT_BOOL Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_StatusText( BSTR *StatusText) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_StatusText( BSTR StatusText) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_ToolBar( int *Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_ToolBar( int Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_MenuBar( VARIANT_BOOL *Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_MenuBar( VARIANT_BOOL Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_FullScreen( VARIANT_BOOL *pbFullScreen) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_FullScreen( VARIANT_BOOL bFullScreen) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Navigate2( VARIANT *URL, VARIANT *Flags, VARIANT *TargetFrameName, VARIANT *PostData, VARIANT *Headers) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryStatusWB( OLECMDID cmdID, OLECMDF *pcmdf) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ExecWB( OLECMDID cmdID, OLECMDEXECOPT cmdexecopt, VARIANT *pvaIn, VARIANT *pvaOut) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ShowBrowserBar( VARIANT *pvaClsid, VARIANT *pvarShow, VARIANT *pvarSize) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_ReadyState( READYSTATE *plReadyState) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Offline( VARIANT_BOOL *pbOffline) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Offline( VARIANT_BOOL bOffline) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Silent( VARIANT_BOOL *pbSilent) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Silent( VARIANT_BOOL bSilent) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_RegisterAsBrowser( VARIANT_BOOL *pbRegister) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_RegisterAsBrowser( VARIANT_BOOL bRegister) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_RegisterAsDropTarget( VARIANT_BOOL *pbRegister) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_RegisterAsDropTarget( VARIANT_BOOL bRegister) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_TheaterMode( VARIANT_BOOL *pbRegister) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_TheaterMode( VARIANT_BOOL bRegister) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_AddressBar( VARIANT_BOOL *Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_AddressBar( VARIANT_BOOL Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE get_Resizable( VARIANT_BOOL *Value) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_Resizable( VARIANT_BOOL Value) override { return E_NOTIMPL; }
};
#pragma warning(pop)
struct ToolSite final : IOleWindow, IServiceProvider {
  ULONG refs = 1;
  HWND parent;
  WebService web;
  int unknownServiceQueries = 0;
  ToolSite(HWND value, Browser *browser) : parent(value), web(browser) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IOleWindow) *out = static_cast<IOleWindow *>(this);
    else if (iid == IID_IServiceProvider) *out = static_cast<IServiceProvider *>(this);
    else return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *out) override { if (!out) return E_POINTER; *out = parent; return S_OK; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryService(REFGUID id, REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (id != IID_IWebBrowserApp || iid != IID_IUnknown) return E_NOINTERFACE;
    ++unknownServiceQueries; return web.QueryInterface(iid, out);
  }
};

static void bandTests() {
  if (!navigation::tabsSupported()) return;
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"CabinetWClass"; cls.lpfnWndProc = DefWindowProcW; RegisterClassW(&cls);
  HWND frame = CreateWindowW(cls.lpszClassName, L"ShibaKit hidden band test", WS_OVERLAPPED, 0, 0, 900, 200, nullptr, nullptr, cls.hInstance, nullptr);
  HWND parent = CreateWindowW(L"STATIC", L"host", WS_CHILD, 0, 0, 900, 100, frame, nullptr, cls.hInstance, nullptr);
  Browser browser; browser.parent = parent;
  ComPtr<IObjectWithSite> site; require(SUCCEEDED(tabs::create(IID_PPV_ARGS(&site))), "create tool band");
  ComPtr<IDeskBand2> band; require(SUCCEEDED(site.As(&band)), "desk band interface");
  ComPtr<IPersistStream> persist; require(SUCCEEDED(site.As(&persist)), "stream persistence interface");
  CLSID id{}; require(SUCCEEDED(persist->GetClassID(&id)) && id == tabs::clsid, "band class identity");
  require(site->SetSite(&browser) == E_ACCESSDENIED, "disabled feature refuses attachment");
  DWORD enabled = 1; RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  require(SUCCEEDED(site->SetSite(&browser)), "attach to isolated Explorer host");
  HWND window = nullptr; require(SUCCEEDED(band->GetWindow(&window)) && window, "band creates child window");
  require(GetPropW(frame, tabs::windowProperty) == window, "activation can find hosted band");
  MoveWindow(window, 0, 0, 1200, 40, FALSE);
  HWND control = GetDlgItem(window, 1);
  auto count = [&] { return TabCtrl_GetItemCount(control); };
  auto title = [&](int index) { wchar_t text[260]{}; TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = text; item.cchTextMax = ARRAYSIZE(text); TabCtrl_GetItem(control, index, &item); return std::wstring(text); };
  auto action = [&](WPARAM verb, LPARAM index = 0) { SendMessageW(window, tabs::actionMessage, verb, index); pump(); };
  require(count() == 1 && title(0) == browser.view.folder.name, "initial tab reads actual folder");
  auto original = browser.view.folder;
  action(tabs::Duplicate); require(count() == 2 && browser.navigations == 0, "duplicate keeps folder without redundant navigation");
  action(tabs::New); require(count() == 3 && browser.view.folder == tabs::computer(), "new tab navigates to This PC");
  action(tabs::Select, 0); require(browser.view.folder == original, "switch returns to first folder");
  auto changed = known(FOLDERID_Profile); browser.view.folder = changed;
  SendMessageW(window, tabs::changedMessage, 0, 0);
  require(title(0) == changed.name && title(1) == original.name, "user browsing updates only active tab");
  browser.outcome = E_ACCESSDENIED;
  action(tabs::Select, 1); require(TabCtrl_GetCurSel(control) == 0 && browser.view.folder == changed, "failed switch preserves selection and location");
  browser.outcome = S_OK; browser.complete = false;
  action(tabs::Select, 1); require(title(1) == original.name, "stale completion cannot overwrite target tab");
  auto requests = browser.navigations; action(tabs::New);
  require(browser.navigations == requests && count() == 3, "no overlapping navigation transactions");
  SendMessageW(window, tabs::failedMessage, 0, 0);
  require(TabCtrl_GetCurSel(control) == 0 && title(0) == changed.name, "asynchronous failure rolls back tab selection");
  action(tabs::Select, 1);
  expirePending(window);
  require(TabCtrl_GetCurSel(control) == 0 && title(0) == changed.name && title(1) == original.name,
          "timeout without completion restores previous selection and preserves target folder");
  action(tabs::Select, 1); browser.view.folder = original;
  expirePending(window);
  require(TabCtrl_GetCurSel(control) == 1 && title(0) == changed.name && title(1) == original.name,
          "timeout after successful navigation retains target without overwriting previous tab");
  browser.complete = true;
  action(tabs::Select, 1); action(tabs::CloseOthers, 1);
  require(count() == 1 && title(0) == original.name, "close others retains selected folder");
  action(tabs::Close); require(count() == 1, "UI retains last tab");
  action(tabs::Restore); require(count() == 2, "UI restores closed tab");
  DESKBANDINFO info{}; info.dwMask = DBIM_MINSIZE | DBIM_MAXSIZE | DBIM_ACTUAL | DBIM_MODEFLAGS;
  require(SUCCEEDED(band->GetBandInfo(7, DBIF_VIEWMODE_NORMAL, &info)) && info.ptMinSize.y == info.ptMaxSize.y, "single fixed-height toolbar row");
  HRESULT wrongThread = S_OK; std::thread thread([&] { wrongThread = site->SetSite(nullptr); }); thread.join();
  require(wrongThread == RPC_E_WRONG_THREAD && IsWindow(window), "wrong thread cannot destroy band");
  browser.onBrowse = [&] { site->SetSite(nullptr); };
  action(tabs::New); require(!IsWindow(window) && !GetPropW(frame, tabs::windowProperty), "reentrant detach during navigation cleans published window");
  browser.onBrowse = {}; browser.onWindow = [&] { site->SetSite(nullptr); };
  require(FAILED(site->SetSite(&browser)), "reentrant detach during site discovery aborts stale setup");
  browser.onWindow = {};
  for (int i = 0; i < 5; ++i) { require(SUCCEEDED(site->SetSite(&browser)), "repeat attachment"); site->SetSite(nullptr); }
  browser.onView = [&] { site->SetSite(nullptr); };
  require(FAILED(site->SetSite(&browser)), "reentrant detach during initial view query aborts stale setup"); browser.onView = {};
  ToolSite toolSite(parent, &browser);
  require(SUCCEEDED(site->SetSite(static_cast<IOleWindow *>(&toolSite))), "toolband site reaches Shell browser through its web browser service");
  require(toolSite.unknownServiceQueries > 0, "toolband service accepting IUnknown is supported");
  require(SUCCEEDED(band->GetWindow(&window)) && IsWindow(window), "indirect browser service creates actual band child");
  wchar_t stage[64]{}; DWORD bytes = sizeof(stage), status = DWORD(E_FAIL), statusBytes = sizeof(status);
  require(RegGetValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"TabsLastBandStage", RRF_RT_REG_SZ, nullptr, stage, &bytes) == ERROR_SUCCESS && !wcscmp(stage, L"attached"), "diagnostics identify successful SetSite stage");
  require(RegGetValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"TabsLastBandResult", RRF_RT_REG_DWORD, nullptr, &status, &statusBytes) == ERROR_SUCCESS && status == S_OK, "diagnostics report actual SetSite result");
  site->SetSite(nullptr); toolSite.web.available = false;
  require(site->SetSite(static_cast<IOleWindow *>(&toolSite)) == E_NOINTERFACE, "missing browser service fails without fake success");
  bytes = sizeof(stage);
  require(RegGetValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"TabsLastBandStage", RRF_RT_REG_SZ, nullptr, stage, &bytes) == ERROR_SUCCESS && !wcscmp(stage, L"browser-service"), "diagnostics locate missing browser service");
  toolSite.web.available = true; toolSite.web.onService = [&] { site->SetSite(nullptr); };
  require(FAILED(site->SetSite(static_cast<IOleWindow *>(&toolSite))), "reentrant detach during web browser fallback aborts stale attachment");
  toolSite.web.onService = {}; site->SetSite(nullptr);
  require(toolSite.refs == 1 && toolSite.web.refs == 1, "toolband and web service references released");
  persist.Reset(); band.Reset(); site.Reset(); pump();
  require(tabs::activeObjects() == 0 && browser.refs == 1 && browser.view.refs == 1, "band windows and COM references released");
  DestroyWindow(frame); UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
struct ControlChanges {
  HWND window;
  unsigned inserts = 0, deletes = 0, deleteAll = 0, itemSizes = 0, geometry = 0;
  std::vector<int> writtenItems;
  explicit ControlChanges(HWND value) : window(value) {
    require(SetWindowSubclass(window, proc, reinterpret_cast<UINT_PTR>(this),
                              reinterpret_cast<DWORD_PTR>(this)) != FALSE, "observe native control changes");
  }
  ~ControlChanges() { if (window) RemoveWindowSubclass(window, proc, reinterpret_cast<UINT_PTR>(this)); }
  void reset() { inserts = deletes = deleteAll = itemSizes = geometry = 0; writtenItems.clear(); }
  bool stableStructure() const { return !inserts && !deletes && !deleteAll && !itemSizes && !geometry; }
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto &self = *reinterpret_cast<ControlChanges *>(data);
    if (message == TCM_INSERTITEMW || message == TCM_INSERTITEMA) ++self.inserts;
    if (message == TCM_DELETEITEM) ++self.deletes;
    if (message == TCM_DELETEALLITEMS) ++self.deleteAll;
    if (message == TCM_SETITEMW || message == TCM_SETITEMA) self.writtenItems.push_back(static_cast<int>(w));
    if (message == TCM_SETITEMSIZE) ++self.itemSizes;
    if (message == WM_WINDOWPOSCHANGING) {
      const auto &position = *reinterpret_cast<WINDOWPOS *>(l);
      if (!(position.flags & SWP_NOMOVE) || !(position.flags & SWP_NOSIZE)) ++self.geometry;
    }
    if (message == WM_NCDESTROY) { self.window = nullptr; RemoveWindowSubclass(window, proc, id); }
    return DefSubclassProc(window, message, w, l);
  }
};
static RECT relativeRect(HWND window, HWND parent) {
  RECT result{}; GetWindowRect(window, &result);
  MapWindowPoints(nullptr, parent, reinterpret_cast<POINT *>(&result), 2); return result;
}
struct TooltipFolder {
  std::wstring root, path;
  TooltipFolder() {
    wchar_t temporary[32768]{}, token[40]{}; GUID id{}; CoCreateGuid(&id);
    require(GetTempPathW(ARRAYSIZE(temporary), temporary) != 0 && StringFromGUID2(id, token, ARRAYSIZE(token)) != 0,
            "choose isolated tooltip folder");
    root = std::wstring(temporary) + L"ShibaKit-Tooltip-" + token;
    path = root + L"\\完整路径_中文_Emoji_\U0001F4C1_&_" + std::wstring(50, L'x');
    require(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "create tooltip fixture root");
    if (!CreateDirectoryW(path.c_str(), nullptr)) { RemoveDirectoryW(root.c_str()); require(false, "create long Unicode tooltip directory"); }
  }
  ~TooltipFolder() { RemoveDirectoryW(path.c_str()); RemoveDirectoryW(root.c_str()); }
  tabs::Location location() const {
    PIDLIST_ABSOLUTE id = nullptr;
    require(SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)), "parse real long Unicode folder PIDL");
    auto result = tabs::location(id); CoTaskMemFree(id);
    // The Shell can expand short names in the environment's temporary path.
    // Verify the PIDL still resolves to the original directory.
    std::error_code error;
    require(std::filesystem::equivalent(path, result.path, error),
            "long Unicode tooltip fixture resolves to the same directory");
    return result;
  }
};
static void stableLayoutTests() {
  if (!navigation::tabsSupported()) return;
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"CabinetWClass"; cls.lpfnWndProc = DefWindowProcW;
  require(RegisterClassW(&cls) != 0, "register stable layout host");
  DWORD enabled = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  for (UINT dpi : {96u, 144u, 192u}) {
    struct DpiScope { UINT previous = simulatedDpi; explicit DpiScope(UINT value) { simulatedDpi = value; } ~DpiScope() { simulatedDpi = previous; } } dpiScope(dpi);
    auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
    HWND frame = CreateWindowW(cls.lpszClassName, L"Hidden stable layout test", WS_OVERLAPPED,
        0, 0, px(1400), px(200), nullptr, nullptr, cls.hInstance, nullptr);
    require(frame != nullptr, "create stable layout frame");
    Browser browser; browser.parent = frame;
    ComPtr<IObjectWithSite> site; require(SUCCEEDED(tabs::create(IID_PPV_ARGS(&site))), "create layout test band");
    require(SUCCEEDED(site->SetSite(&browser)), "attach layout test band");
    ComPtr<IDeskBand2> band; require(SUCCEEDED(site.As(&band)), "get layout band interface");
    HWND window = nullptr; require(SUCCEEDED(band->GetWindow(&window)) && window, "get layout band window");
    MoveWindow(window, 0, 0, px(1200), px(40), FALSE);
    HWND control = GetDlgItem(window, 1), overflow = GetDlgItem(window, 3);
    require(!GetDlgItem(window, 2), "tab strip no longer contains a plus button");
    LOGFONTW labelFont{};
    require(GetObjectW(reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)), sizeof(labelFont), &labelFont) == sizeof(labelFont) &&
            labelFont.lfHeight == -px(12), "tab label uses compact 12 logical pixel font at each DPI");
    const HWND tooltip = TabCtrl_GetToolTips(control);
    require(IsWindow(tooltip), "native tab tooltip window exists");
    auto tip = [&](UINT_PTR row) {
      NMTTDISPINFOW request{}; request.hdr.hwndFrom = tooltip; request.hdr.idFrom = row; request.hdr.code = TTN_GETDISPINFOW;
      SendMessageW(window, WM_NOTIFY, row, reinterpret_cast<LPARAM>(&request));
      require(request.lpszText && request.lpszText != LPSTR_TEXTCALLBACKW && !(request.uFlags & TTF_DI_SETITEM),
              "tooltip supplies live text without caching a stale directory");
      return std::wstring(request.lpszText);
    };
    auto title = [&](int index) { wchar_t text[260]{}; TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = text; item.cchTextMax = ARRAYSIZE(text); TabCtrl_GetItem(control, index, &item); return std::wstring(text); };
    const auto original = browser.view.folder;
    SendMessageW(window, tabs::actionMessage, tabs::Duplicate, 0); pump();
    require(TabCtrl_GetItemCount(control) == 2, "layout fixture has two independent tabs");
    require(tip(0) == original.path && tip(1) == original.path, "tooltip rows resolve to their complete folder paths");
    ControlChanges tabChanges(control), overflowChanges(overflow);
    RECT tabBounds = relativeRect(control, window), overflowBounds = relativeRect(overflow, window), firstItem{};
    TabCtrl_GetItemRect(control, 0, &firstItem);
    require(abs((firstItem.right - firstItem.left) - px(190)) <= 2, "wide layouts retain fixed logical tab width at each DPI");
    for (int repeat = 0; repeat < 5; ++repeat) SendMessageW(window, tabs::changedMessage, 0, 0);
    pump();
    require(tabChanges.stableStructure() && tabChanges.writtenItems.empty() &&
            overflowChanges.stableStructure(),
            "unchanged navigation refresh never rebuilds or repositions native controls");
    browser.view.folder = known(FOLDERID_Profile);
    SendMessageW(window, tabs::changedMessage, 0, 0); pump();
    RECT afterTab = relativeRect(control, window), afterOverflow = relativeRect(overflow, window);
    require(tabChanges.stableStructure() && tabChanges.writtenItems == std::vector<int>{1} &&
            overflowChanges.stableStructure() && EqualRect(&tabBounds, &afterTab) && EqualRect(&overflowBounds, &afterOverflow),
            "folder change updates only active title without moving tab strip or overflow button");
    require(title(0) == original.name && title(1) == browser.view.folder.name,
            "folder rename preserves the inactive tab title");
    require(tip(0) == original.path && tip(1) == browser.view.folder.path,
            "navigation updates only the corresponding tooltip path");
    auto switchWithReentrantLayout = [&](int target, HRESULT outcome) {
      const int previous = TabCtrl_GetCurSel(control);
      const auto firstTitle = title(0), secondTitle = title(1);
      const auto beforeTabs = relativeRect(control, window), beforeOverflow = relativeRect(overflow, window);
      tabChanges.reset(); overflowChanges.reset();
      std::vector<int> selections;
      bool titlesStable = true;
      auto sample = [&] {
        selections.push_back(TabCtrl_GetCurSel(control));
        titlesStable = titlesStable && title(0) == firstTitle && title(1) == secondTitle;
      };
      browser.outcome = outcome;
      browser.onBrowse = [&] {
        sample();
        RECT client{}; GetClientRect(window, &client);
        SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom)); sample();
        SendMessageW(window, tabs::changedMessage, 0, 0); sample();
        pump(); sample();
      };
      // The native tab changes its selection before notifying its parent. The
      // Shell navigation can then pump size/completion messages synchronously.
      TabCtrl_SetCurSel(control, target);
      NMHDR changed{control, 1, TCN_SELCHANGE};
      SendMessageW(window, WM_NOTIFY, 1, reinterpret_cast<LPARAM>(&changed)); pump();
      browser.onBrowse = {}; browser.outcome = S_OK;
      const bool heldTarget = !selections.empty() && std::all_of(selections.begin(), selections.end(),
                                                               [target](int selected) { return selected == target; });
      if (!heldTarget) {
        std::cerr << "Reentrant switch target=" << target << " samples=";
        for (int selected : selections) std::cerr << selected << ',';
        std::cerr << '\n';
      }
      require(heldTarget && titlesStable, "all reentrant navigation samples retain target selection and existing tab titles");
      const auto afterTabs = relativeRect(control, window), afterOverflow = relativeRect(overflow, window);
      require(tabChanges.stableStructure() && tabChanges.writtenItems.empty() && overflowChanges.stableStructure() &&
              EqualRect(&beforeTabs, &afterTabs) && EqualRect(&beforeOverflow, &afterOverflow),
              "success and failure navigation do not rebuild or reposition existing tab controls");
      require(TabCtrl_GetCurSel(control) == (SUCCEEDED(outcome) ? target : previous),
              "successful switch stays selected and failed switch rolls back only after BrowseObject returns");
    };
    switchWithReentrantLayout(0, S_OK);
    switchWithReentrantLayout(1, E_ACCESSDENIED);
    SendMessageW(window, tabs::actionMessage, tabs::Select, 1); pump();
    require(TabCtrl_GetCurSel(control) == 1 && title(0) == original.name && title(1) == browser.view.folder.name,
            "navigation regression fixture restores active folder without changing other tabs");
    const auto latestFolder = browser.view.folder;
    browser.view.folder = original;
    SendMessageW(window, tabs::changedMessage, 0, 0);
    tabChanges.reset(); overflowChanges.reset();
    const auto beforeNestedTabs = relativeRect(control, window), beforeNestedOverflow = relativeRect(overflow, window);
    bool nestedLatest = false; int nestedSelection = -1;
    browser.view.onGetFolder = [&] {
      browser.view.folder = latestFolder;
      SendMessageW(window, tabs::changedMessage, 0, 0);
      nestedLatest = title(1) == latestFolder.name;
      nestedSelection = TabCtrl_GetCurSel(control);
    };
    SendMessageW(window, tabs::changedMessage, 0, 0);
    const bool outerLatest = title(1) == latestFolder.name;
    if (!nestedLatest || !outerLatest)
      std::cerr << "Nested folder refresh: innerLatest=" << nestedLatest << " outerLatest=" << outerLatest
                << " selected=" << TabCtrl_GetCurSel(control) << '\n';
    require(nestedLatest && outerLatest && tip(1) == latestFolder.path && title(0) == original.name &&
            nestedSelection == 1 && TabCtrl_GetCurSel(control) == 1,
            "older outer folder query cannot overwrite newer nested title or tooltip and selection stays stable");
    const auto afterNestedTabs = relativeRect(control, window), afterNestedOverflow = relativeRect(overflow, window);
    require(tabChanges.stableStructure() && tabChanges.writtenItems == std::vector<int>{1} &&
            overflowChanges.stableStructure() && EqualRect(&beforeNestedTabs, &afterNestedTabs) &&
            EqualRect(&beforeNestedOverflow, &afterNestedOverflow),
            "nested folder completion updates only current title once without rebuilding or moving tabs");
    pump();
    const auto activeTitle = title(1);
    auto count = [&] { return TabCtrl_GetItemCount(control); };
    auto background = [&] {
      const auto bounds = relativeRect(control, window), button = relativeRect(overflow, window);
      require(button.left - bounds.right >= px(32), "at least 32 logical pixels of blank strip remain before overflow");
      return POINT{(bounds.right + button.left) / 2, px(15)};
    };
    auto tail = [&] {
      RECT last{}, bounds{}; TabCtrl_GetItemRect(control, count() - 1, &last); GetClientRect(control, &bounds);
      require(last.right < bounds.right, "native tab control has a hittable trailing blank area");
      return POINT{bounds.right - 1, px(15)};
    };
    auto mouse = [&](HWND target, UINT message, POINT point, WPARAM keys = 0) {
      SendMessageW(target, message, keys, MAKELPARAM(point.x, point.y));
    };
    auto click = [&](HWND target, POINT point, WPARAM keys = 0) {
      mouse(target, WM_MOUSEMOVE, point, keys);
      mouse(target, WM_LBUTTONDOWN, point, keys | MK_LBUTTON);
      mouse(target, WM_LBUTTONUP, point, keys); pump();
    };
    const auto duplicatedFolder = browser.view.folder;
    const auto navigationCount = browser.navigations;
    tabChanges.reset(); overflowChanges.reset();
    background(); // Also verify the reserved clickable area before the first duplicate.
    const POINT firstBlank{tabBounds.right + px(190) - px(17), px(15)};
    click(window, firstBlank);
    RECT firstAfter{}; TabCtrl_GetItemRect(control, 0, &firstAfter);
    require(count() == 3 && TabCtrl_GetCurSel(control) == 2 && browser.view.folder == duplicatedFolder &&
            browser.navigations == navigationCount && title(0) == original.name && title(1) == activeTitle && title(2) == activeTitle,
            "band background click duplicates current folder, activates it and preserves existing tabs");
    require(tabChanges.inserts == 1 && !tabChanges.deletes && !tabChanges.deleteAll &&
            EqualRect(&firstItem, &firstAfter), "adding one tab inserts one item without shifting earlier tabs");
    POINT covered = firstBlank; MapWindowPoints(window, control, &covered, 1);
    RECT newItem{}; TabCtrl_GetItemRect(control, 2, &newItem);
    require(covered.x >= newItem.right - px(23) && covered.x < newItem.right - px(3),
            "new tab close target covers the previous blank click point");
    mouse(control, WM_LBUTTONDBLCLK, covered, MK_LBUTTON);
    mouse(control, WM_LBUTTONUP, covered); pump();
    require(count() == 3 && TabCtrl_GetCurSel(control) == 2,
            "second half of blank double click cannot close newly appeared tab under the pointer");
    click(control, tail());
    require(count() == 4 && TabCtrl_GetCurSel(control) == 3 && browser.view.folder == duplicatedFolder &&
            browser.navigations == navigationCount && title(3) == activeTitle,
            "tab control trailing blank click duplicates and activates the current folder");
    RECT item{}; TabCtrl_GetItemRect(control, 0, &item);
    click(control, {item.left + px(35), (item.top + item.bottom) / 2});
    require(count() == 4 && TabCtrl_GetCurSel(control) == 0 && browser.view.folder == original,
            "clicking a real tab selects it without creating another tab");
    TabCtrl_GetItemRect(control, 3, &item);
    click(control, {item.right - px(12), (item.top + item.bottom) / 2});
    require(count() == 3, "clicking a tab close target removes one tab without duplicating");
    const int unchangedCount = count();
    const auto blank = background();
    const POINT dragged{blank.x + GetSystemMetrics(SM_CXDRAG) + px(20), blank.y};
    mouse(window, WM_LBUTTONDOWN, blank, MK_LBUTTON);
    mouse(window, WM_MOUSEMOVE, dragged, MK_LBUTTON);
    mouse(window, WM_MOUSEMOVE, blank, MK_LBUTTON);
    mouse(window, WM_LBUTTONUP, blank); pump();
    require(count() == unchangedCount, "dragging out and back cannot become a blank-area click");
    for (WPARAM keys : {WPARAM(MK_CONTROL), WPARAM(MK_SHIFT)}) click(window, blank, keys);
    for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) {
      simulatedModifier = key; click(window, blank); simulatedModifier = 0;
    }
    require(count() == unchangedCount, "Ctrl Shift Alt and Windows modified clicks do not duplicate");
    mouse(window, WM_LBUTTONDOWN, blank, MK_LBUTTON);
    SendMessageW(window, WM_CANCELMODE, 0, 0);
    mouse(window, WM_LBUTTONUP, blank); pump();
    require(count() == unchangedCount, "cancelled blank click does not duplicate");
    mouse(window, WM_LBUTTONDOWN, blank, MK_LBUTTON);
    SetCapture(frame); ReleaseCapture();
    mouse(window, WM_LBUTTONUP, blank); pump();
    require(count() == unchangedCount, "losing mouse capture cancels pending blank click");
    mouse(window, WM_LBUTTONDOWN, blank, MK_LBUTTON);
    mouse(control, WM_LBUTTONUP, tail());
    SendMessageW(window, WM_CANCELMODE, 0, 0); ReleaseCapture(); pump();
    require(count() == unchangedCount, "down and up in different windows do not duplicate");
    mouse(window, WM_LBUTTONDOWN, blank, MK_LBUTTON);
    browser.view.folder = known(FOLDERID_Profile);
    SendMessageW(window, tabs::changedMessage, 0, 0);
    mouse(window, WM_LBUTTONUP, blank); pump();
    require(count() == unchangedCount, "folder navigation during mouse press invalidates duplication candidate");
    browser.view.folder = original;
    SendMessageW(window, tabs::changedMessage, 0, 0); pump();
    const int beforeNew = count(), previousSelection = TabCtrl_GetCurSel(control);
    SendMessageW(window, tabs::actionMessage, tabs::New, 0); pump();
    require(count() == beforeNew + 1 && TabCtrl_GetCurSel(control) == previousSelection + 1 &&
            browser.view.folder == tabs::computer() && tip(TabCtrl_GetCurSel(control)) == tabs::computer().name,
            "explicit new tab opens This PC after active tab and tooltip falls back to display name");
    {
      TooltipFolder firstPath, secondPath;
      SendMessageW(window, tabs::actionMessage, tabs::Select, 1); pump();
      const auto firstFolder = firstPath.location();
      browser.view.folder = firstFolder;
      SendMessageW(window, tabs::changedMessage, 0, 0); pump();
      require(firstFolder.path.size() > 80 && tip(1) == firstFolder.path,
              "tooltip does not truncate a long path containing Chinese, emoji and ampersand");
      const auto unchangedTitle = title(1);
      const auto secondFolder = secondPath.location();
      browser.view.folder = secondFolder;
      SendMessageW(window, tabs::changedMessage, 0, 0); pump();
      require(title(1) == unchangedTitle && tip(1) == secondFolder.path && tip(0) == original.path,
              "same-title folder navigation replaces tooltip path without stale cache or affecting other tabs");
      browser.view.folder = original;
      SendMessageW(window, tabs::changedMessage, 0, 0); pump();
    }
    MoveWindow(window, 0, 0, px(8000), px(40), FALSE);
    SendMessageW(window, tabs::actionMessage, tabs::Select, count() - 1); pump();
    while (count() < int(tabs::Model::limit)) {
      const int previous = count(); click(window, background());
      require(count() == previous + 1 && TabCtrl_GetCurSel(control) == previous,
              "blank click adds and activates exactly one tab before limit");
    }
    const auto lastFolder = browser.view.folder;
    click(window, background()); click(control, tail());
    require(count() == int(tabs::Model::limit) && TabCtrl_GetCurSel(control) == int(tabs::Model::limit) - 1 && browser.view.folder == lastFolder,
            "blank clicks respect 32-tab limit without changing active folder");
    site->SetSite(nullptr); band.Reset(); site.Reset(); pump();
    require(tabs::activeObjects() == 0 && browser.refs == 1 && browser.view.refs == 1,
            "layout tests release hidden windows and browser references");
    DestroyWindow(frame);
  }
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
static void rollbackViewportTests() {
  if (!navigation::tabsSupported()) return;
  struct DpiScope { UINT previous = simulatedDpi; DpiScope() { simulatedDpi = 96; } ~DpiScope() { simulatedDpi = previous; } } dpiScope;
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"CabinetWClass"; cls.lpfnWndProc = DefWindowProcW;
  require(RegisterClassW(&cls) != 0, "register overflow rollback host");
  DWORD enabled = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  HWND frame = CreateWindowW(cls.lpszClassName, L"Hidden overflow rollback test", WS_OVERLAPPED,
      0, 0, 1400, 200, nullptr, nullptr, cls.hInstance, nullptr);
  require(frame != nullptr, "create overflow rollback frame");
  Browser browser; browser.parent = frame;
  const std::vector<tabs::Location> folders{
      browser.view.folder, known(FOLDERID_System), known(FOLDERID_Profile),
      known(FOLDERID_RoamingAppData), known(FOLDERID_LocalAppData), known(FOLDERID_ProgramData)};
  ComPtr<IObjectWithSite> site;
  require(SUCCEEDED(tabs::create(IID_PPV_ARGS(&site))) && SUCCEEDED(site->SetSite(&browser)), "attach overflow rollback band");
  ComPtr<IDeskBand2> band; require(SUCCEEDED(site.As(&band)), "get overflow rollback band");
  HWND window = nullptr;
  require(SUCCEEDED(band->GetWindow(&window)) && window, "get overflow rollback window");
  MoveWindow(window, 0, 0, 1200, 40, FALSE);
  HWND control = GetDlgItem(window, 1), overflow = GetDlgItem(window, 3);
  auto title = [&](int row) {
    wchar_t text[260]{}; TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = text; item.cchTextMax = ARRAYSIZE(text);
    TabCtrl_GetItem(control, row, &item); return std::wstring(text);
  };
  auto action = [&](WPARAM verb, LPARAM index) { SendMessageW(window, tabs::actionMessage, verb, index); pump(); };
  for (size_t index = 1; index < folders.size(); ++index) {
    action(tabs::Duplicate, index - 1);
    browser.view.folder = folders[index];
    SendMessageW(window, tabs::changedMessage, 0, 0);
  }
  require(TabCtrl_GetItemCount(control) == 6, "overflow rollback fixture retains six distinct folders");
  // Width admits exactly three tabs. Selecting the first visible tab leaves
  // first=3 and active=3; selecting index 0 uses the same path as overflow menu.
  MoveWindow(window, 0, 0, 488, 40, FALSE);
  action(tabs::Select, 3);
  auto viewportIs = [&](size_t first) {
    return TabCtrl_GetItemCount(control) == 3 && title(0) == folders[first].name &&
        title(1) == folders[first + 1].name && title(2) == folders[first + 2].name;
  };
  require(viewportIs(3) && TabCtrl_GetCurSel(control) == 0 && browser.view.folder == folders[3],
          "fixture starts with tabs 3/4/5 visible and tab 3 selected");
  const auto beforeTabs = relativeRect(control, window), beforeOverflow = relativeRect(overflow, window);
  auto checkRollback = [&](const char *message) {
    require(viewportIs(3) && TabCtrl_GetCurSel(control) == 0 && browser.view.folder == folders[3], message);
    const auto afterTabs = relativeRect(control, window), afterOverflow = relativeRect(overflow, window);
    require(EqualRect(&beforeTabs, &afterTabs) && EqualRect(&beforeOverflow, &afterOverflow),
            "rollback restores original visible row without moving controls");
  };
  bool publishedTarget = false;
  browser.outcome = E_ACCESSDENIED;
  browser.onBrowse = [&] { publishedTarget = viewportIs(0) && TabCtrl_GetCurSel(control) == 0; };
  action(tabs::Select, 0);
  browser.onBrowse = {};
  require(publishedTarget, "offscreen selection publishes the target viewport before Shell navigation");
  checkRollback("synchronous failure restores first=3 rather than scrolling to first=1");

  browser.outcome = S_OK; browser.complete = false;
  action(tabs::Select, 0);
  require(viewportIs(0), "pending offscreen navigation displays destination viewport");
  expirePending(window);
  checkRollback("asynchronous timeout restores the exact original viewport and active folder");

  action(tabs::Select, 0);
  require(viewportIs(0), "second pending offscreen selection can start after rollback");
  SendMessageW(window, tabs::failedMessage, 0, 0);
  checkRollback("asynchronous error also restores original viewport instead of unrelated titles");

  // While the old pending query is returning its captured folder, Explorer may
  // complete that navigation and start another one on the same browser site.
  // The old resolver must not shorten the newer navigation's timer to 250 ms.
  action(tabs::Select, 0);
  bool oldCompleted = false, newStarted = false;
  browser.view.onGetFolder = [&] {
    browser.view.folder = folders[0];
    SendMessageW(window, tabs::changedMessage, 0, 0);
    oldCompleted = viewportIs(0) && TabCtrl_GetCurSel(control) == 0;
    action(tabs::Select, 2);
    newStarted = viewportIs(0) && TabCtrl_GetCurSel(control) == 2;
  };
  timerDelays.clear(); observedTimerWindow = window;
  expirePending(window);
  observedTimerWindow = nullptr;
  require(oldCompleted && newStarted, "old pending folder query reenters completion followed by a newer tab navigation");
  require(timerDelays == std::vector<UINT>{15000},
          "stale pending resolver preserves newer navigation's full timeout instead of replacing it with 250 ms");
  require(viewportIs(0) && TabCtrl_GetCurSel(control) == 2 && browser.view.folder == folders[0],
          "new pending selection and titles survive the older folder query returning late");
  browser.view.folder = folders[2];
  SendMessageW(window, tabs::changedMessage, 0, 0);
  require(viewportIs(0) && TabCtrl_GetCurSel(control) == 2 && title(0) == folders[0].name &&
          title(2) == folders[2].name, "newer navigation completes without overwriting the previous tab title");
  browser.complete = true;
  site->SetSite(nullptr); band.Reset(); site.Reset(); pump();
  require(tabs::activeObjects() == 0 && browser.refs == 1 && browser.view.refs == 1,
          "overflow rollback fixture releases windows and browser references");
  DestroyWindow(frame); UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
struct CaptureReentry {
  std::function<void()> callback;
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto &self = *reinterpret_cast<CaptureReentry *>(data);
    if (message == WM_CAPTURECHANGED && self.callback) { auto callback = std::move(self.callback); callback(); }
    return DefSubclassProc(window, message, w, l);
  }
};
static void tabInputTests() {
  if (!navigation::tabsSupported()) return;
  struct DpiScope { UINT previous = simulatedDpi; DpiScope() { simulatedDpi = 96; } ~DpiScope() { simulatedDpi = previous; } } dpiScope;
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"CabinetWClass"; cls.lpfnWndProc = DefWindowProcW;
  require(RegisterClassW(&cls) != 0, "register tab input fixture");
  DWORD enabled = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  HWND frame = CreateWindowW(cls.lpszClassName, L"Hidden tab input test", WS_OVERLAPPED, 0, 0, 1400, 200, nullptr, nullptr, cls.hInstance, nullptr);
  Browser browser; browser.parent = frame;
  const std::vector<tabs::Location> folders{browser.view.folder, known(FOLDERID_Profile), known(FOLDERID_RoamingAppData), known(FOLDERID_LocalAppData)};
  ComPtr<IObjectWithSite> site;
  require(SUCCEEDED(tabs::create(IID_PPV_ARGS(&site))) && SUCCEEDED(site->SetSite(&browser)), "attach tab input band");
  ComPtr<IDeskBand2> band; require(SUCCEEDED(site.As(&band)), "get tab input band");
  HWND window = nullptr; require(SUCCEEDED(band->GetWindow(&window)), "get tab input window");
  MoveWindow(window, 0, 0, 1200, 40, FALSE);
  HWND control = GetDlgItem(window, 1);
  auto action = [&](unsigned verb, size_t index) { SendMessageW(window, tabs::actionMessage, verb, index); pump(); };
  auto count = [&] { return TabCtrl_GetItemCount(control); };
  for (size_t index = 1; index < folders.size(); ++index) {
    action(tabs::Duplicate, index - 1); browser.view.folder = folders[index];
    SendMessageW(window, tabs::changedMessage, 0, 0);
  }
  auto point = [&](int row, bool close = false) {
    RECT item{}; TabCtrl_GetItemRect(control, row, &item);
    return POINT{close ? item.right - 12 : item.left + 35, (item.top + item.bottom) / 2};
  };
  auto mouse = [&](UINT message, POINT where, WPARAM keys = 0) { SendMessageW(control, message, keys, MAKELPARAM(where.x, where.y)); };
  auto wheel = [&](int delta, bool forwarded = false, POINT where = POINT{-1, -1}, WPARAM keys = 0) {
    if (where.x == -1) where = point(0);
    ClientToScreen(control, &where);
    return SendMessageW(forwarded ? window : control, forwarded ? tabs::wheelInputMessage : WM_MOUSEWHEEL,
                        MAKEWPARAM(keys, static_cast<WORD>(delta)), MAKELPARAM(where.x, where.y));
  };
  const int firstNavigations = browser.navigations;
  wheel(60); pump(); require(TabCtrl_GetCurSel(control) == 3 && browser.navigations == firstNavigations, "half wheel notch does not navigate yet");
  require(wheel(60, true) == TRUE, "forwarded wheel over a real tab is consumed"); pump();
  require(TabCtrl_GetCurSel(control) == 2 && browser.view.folder == folders[2], "two half notches select previous tab once");
  auto before = browser.navigations;
  wheel(360); pump();
  require(TabCtrl_GetCurSel(control) == 0 && browser.navigations == before + 1, "multi-step wheel clamps to first tab with one navigation");
  wheel(120); pump(); require(browser.navigations == before + 1, "wheel does not wrap before first tab");
  wheel(-480); pump(); require(TabCtrl_GetCurSel(control) == 3 && browser.navigations == before + 2, "negative multi-step wheel selects last tab once");
  wheel(-120); pump(); require(browser.navigations == before + 2, "wheel does not wrap past last tab");
  RECT bounds{}; GetClientRect(control, &bounds);
  wheel(60); require(wheel(120, true, {bounds.right + 10, 15}) == FALSE, "wheel over blank strip is not consumed");
  wheel(60); pump(); require(TabCtrl_GetCurSel(control) == 3, "leaving actual tabs discards fractional wheel delta");
  wheel(60); pump(); require(TabCtrl_GetCurSel(control) == 2, "fractional accumulation restarts after returning to a tab");
  require(wheel(120, true, point(0), MK_CONTROL) == FALSE, "modified forwarded wheel is not treated as tab switching");

  action(tabs::Select, 0); browser.complete = false; before = browser.navigations;
  wheel(-120); pump();
  wheel(-120); wheel(-120); wheel(120); pump();
  require(browser.navigations == before + 1 && TabCtrl_GetCurSel(control) == 1,
          "busy wheel coalesces latest target without changing in-flight selection or overlapping navigation");
  SendMessageW(window, tabs::failedMessage, WPARAM(-1), 0);
  SendMessageW(window, WM_TIMER, 1, 0);
  require(TabCtrl_GetCurSel(control) == 1, "stale failure and early timer cannot roll back current wheel navigation");
  browser.view.folder = folders[1]; SendMessageW(window, tabs::changedMessage, 0, 0); pump();
  require(browser.navigations == before + 2 && TabCtrl_GetCurSel(control) == 2 && browser.view.folder == folders[1],
          "completion drains exactly latest queued wheel target while preserving current actual view");
  browser.view.folder = folders[2]; SendMessageW(window, tabs::changedMessage, 0, 0); pump();
  require(TabCtrl_GetCurSel(control) == 2 && browser.navigations == before + 2, "queued destination settles without extra navigation");
  browser.complete = true;
  bool nested = false; before = browser.navigations;
  browser.onBrowse = [&] { if (!nested) { nested = true; wheel(-240); wheel(-120); pump(); } };
  wheel(120); pump(); browser.onBrowse = {};
  require(nested && TabCtrl_GetCurSel(control) == 3 && browser.navigations == before + 2 && browser.maximumBrowseDepth == 1,
          "wheel input pumped inside BrowseObject queues one latest target and never reenters navigation");

  action(tabs::Select, 0); browser.complete = false; before = browser.navigations;
  wheel(-120); pump(); wheel(-240); pump();
  SendMessageW(window, tabs::failedMessage, 0, 0); pump();
  require(TabCtrl_GetCurSel(control) == 0 && browser.navigations == before + 1, "failed wheel navigation discards queued target after rollback");
  wheel(-120); pump(); wheel(-240); pump(); expirePending(window); pump();
  require(TabCtrl_GetCurSel(control) == 0 && browser.navigations == before + 2, "timed-out wheel navigation also discards queued target");
  browser.complete = true; browser.outcome = E_ACCESSDENIED;
  browser.onBrowse = [&] { wheel(-240); };
  wheel(-120); pump(); browser.onBrowse = {}; browser.outcome = S_OK;
  require(TabCtrl_GetCurSel(control) == 0 && browser.navigations == before + 3, "immediate failure cancels wheel queued during BrowseObject");

  action(tabs::Select, 3);
  auto middle = [&](POINT down, POINT up, WPARAM keys = 0, UINT downMessage = WM_MBUTTONDOWN) {
    mouse(downMessage, down, keys | MK_MBUTTON); mouse(WM_MBUTTONUP, up, keys); pump();
  };
  mouse(WM_MBUTTONUP, point(0)); pump(); require(count() == 4, "unpaired middle release cannot close a tab");
  middle(point(0), point(1)); require(count() == 4 && TabCtrl_GetCurSel(control) == 3, "middle release over another tab cancels without selecting background tab");
  mouse(WM_MBUTTONDOWN, point(0), MK_MBUTTON);
  auto dragged = point(0); dragged.x += GetSystemMetrics(SM_CXDRAG) + 20;
  mouse(WM_MOUSEMOVE, dragged, MK_MBUTTON); mouse(WM_MOUSEMOVE, point(0), MK_MBUTTON); mouse(WM_MBUTTONUP, point(0)); pump();
  require(count() == 4, "middle drag out and back cannot close a tab");
  for (WPARAM keys : {WPARAM(MK_CONTROL), WPARAM(MK_SHIFT)}) middle(point(0), point(0), keys);
  for (int key : {VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN, VK_RWIN}) {
    simulatedModifier = key; middle(point(0), point(0)); simulatedModifier = 0;
  }
  require(count() == 4, "modified middle clicks leave tabs untouched");
  mouse(WM_MBUTTONDOWN, point(0), MK_MBUTTON); SetCapture(frame); ReleaseCapture(); mouse(WM_MBUTTONUP, point(0)); pump();
  require(count() == 4, "lost middle capture cancels close");
  mouse(WM_MBUTTONDOWN, point(0), MK_MBUTTON); SendMessageW(control, WM_CANCELMODE, 0, 0); mouse(WM_MBUTTONUP, point(0)); pump();
  require(count() == 4, "cancel mode cancels middle close");
  mouse(WM_MBUTTONDOWN, point(0), MK_MBUTTON); browser.view.folder = folders[1];
  SendMessageW(window, tabs::changedMessage, 0, 0); mouse(WM_MBUTTONUP, point(0)); pump();
  require(count() == 4, "folder refresh while middle held invalidates its close target");
  browser.view.folder = folders[3]; SendMessageW(window, tabs::changedMessage, 0, 0);
  mouse(WM_MBUTTONDOWN, point(0), MK_MBUTTON); action(tabs::Select, 1); mouse(WM_MBUTTONUP, point(0)); pump();
  require(count() == 4, "tab navigation while middle held invalidates close"); action(tabs::Select, 3);
  CaptureReentry reentry;
  require(SetWindowSubclass(control, CaptureReentry::proc, 94, reinterpret_cast<DWORD_PTR>(&reentry)) != FALSE, "install middle capture reentry observer");
  reentry.callback = [&] { action(tabs::Select, 1); };
  middle(point(0), point(0));
  require(count() == 4 && TabCtrl_GetCurSel(control) == 1, "navigation during capture release prevents stale middle close");
  RemoveWindowSubclass(control, CaptureReentry::proc, 94); action(tabs::Select, 3);
  mouse(WM_MOUSEMOVE, point(0, true)); SendMessageW(control, WM_MOUSELEAVE, 0, 0);
  mouse(WM_LBUTTONDOWN, point(0, true), MK_LBUTTON); mouse(WM_LBUTTONUP, point(0, true)); pump();
  require(count() == 4, "hidden close region after mouse leave selects rather than closes a tab");
  action(tabs::Select, 3); before = browser.navigations;
  mouse(WM_MOUSEMOVE, point(0, true)); mouse(WM_LBUTTONDOWN, point(0, true), MK_LBUTTON); mouse(WM_LBUTTONUP, point(0, true)); pump();
  require(count() == 3 && browser.navigations == before && browser.view.folder == folders[3], "hovered X closes background tab without navigation");
  middle(point(0), point(0));
  require(count() == 2 && TabCtrl_GetCurSel(control) == 1 && browser.navigations == before && browser.view.folder == folders[3],
          "paired middle click closes only hovered background tab and keeps active folder");
  middle(point(0), point(0), 0, WM_MBUTTONDBLCLK);
  require(count() == 1 && TabCtrl_GetCurSel(control) == 0 && browser.navigations == before, "second middle double-click pair can close next hovered tab");
  middle(point(0), point(0)); require(count() == 1, "middle click retains last tab");
  site->SetSite(nullptr); band.Reset(); site.Reset(); pump();
  require(tabs::activeObjects() == 0 && browser.refs == 1 && browser.view.refs == 1, "tab input fixture releases windows and COM references");
  DestroyWindow(frame); UnregisterClassW(cls.lpszClassName, cls.hInstance);
}
static void tabDropTests() {
  if (!navigation::tabsSupported()) return;
  TooltipFolder destinationFolder;
  const auto sourceFile = destinationFolder.root + L"\\source.txt";
  const auto copiedFile = destinationFolder.path + L"\\source.txt";
  struct FileCleanup {
    const std::wstring &source, &copy;
    ~FileCleanup() { DeleteFileW(copy.c_str()); DeleteFileW(source.c_str()); }
  } cleanup{sourceFile, copiedFile};
  auto file = CreateFileW(sourceFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  require(file != INVALID_HANDLE_VALUE, "create isolated drag source file");
  const char content[] = "ShibaKit drop test";
  DWORD written = 0;
  const bool writeOk = WriteFile(file, content, sizeof(content), &written, nullptr) && written == sizeof(content);
  CloseHandle(file);
  require(writeOk, "write isolated drag source file");
  struct Fixture {
    WNDCLASSW cls{};
    HWND frame = nullptr;
    Browser browser;
    RecordingDropParent parent;
    ComPtr<IObjectWithSite> site;
    ~Fixture() {
      if (site) site->SetSite(nullptr);
      observedDropTarget.Reset();
      observeDropTarget = false;
      dropParent = nullptr;
      if (frame) DestroyWindow(frame);
      if (cls.lpszClassName) UnregisterClassW(cls.lpszClassName, cls.hInstance);
    }
  } fixture;
  fixture.cls.hInstance = GetModuleHandleW(nullptr);
  fixture.cls.lpszClassName = L"CabinetWClass";
  fixture.cls.lpfnWndProc = DefWindowProcW;
  require(RegisterClassW(&fixture.cls) != 0, "register isolated tab drop host");
  DWORD enabled = 1;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  fixture.frame = CreateWindowW(fixture.cls.lpszClassName, L"Hidden tab drop test", WS_OVERLAPPED,
      0, 0, 1000, 200, nullptr, nullptr, fixture.cls.hInstance, nullptr);
  require(fixture.frame != nullptr, "create isolated tab drop frame");
  fixture.browser.parent = fixture.frame;
  observeDropTarget = true;
  dropParent = &fixture.parent;
  fixture.parent.locations = {fixture.browser.view.folder, known(FOLDERID_Profile)};
  require(SUCCEEDED(tabs::create(IID_PPV_ARGS(&fixture.site))) && SUCCEEDED(fixture.site->SetSite(&fixture.browser)),
          "attach tab drop fixture");
  require(observedDropTarget != nullptr, "attached tab strip registers its drop target with OLE");
  ComPtr<IDeskBand2> band;
  HWND window = nullptr;
  require(SUCCEEDED(fixture.site.As(&band)) && SUCCEEDED(band->GetWindow(&window)), "get isolated tab drop window");
  MoveWindow(window, 0, 0, 900, 40, FALSE);
  const HWND control = GetDlgItem(window, 1);
  SendMessageW(window, tabs::actionMessage, tabs::Duplicate, 0);
  pump();
  fixture.browser.view.folder = fixture.parent.locations[1];
  SendMessageW(window, tabs::changedMessage, 0, 0);
  require(TabCtrl_GetItemCount(control) == 2 && TabCtrl_GetCurSel(control) == 1,
          "fixture has two distinct tabs with the second active");
  const auto point = [&](int row) {
    RECT bounds{};
    require(TabCtrl_GetItemRect(control, row, &bounds) != FALSE, "find actual tab drop bounds");
    POINT value{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    ClientToScreen(control, &value);
    return POINTL{value.x, value.y};
  };
  auto sourceItem = reinterpret_cast<PCIDLIST_ABSOLUTE>(fixture.parent.locations[1].id.data());
  ComPtr<IShellFolder> sourceParent;
  PCUITEMID_CHILD child = nullptr;
  require(SUCCEEDED(SHBindToParent(sourceItem, IID_PPV_ARGS(&sourceParent), &child)) && child,
          "bind Shell source item");
  ComPtr<IDataObject> data;
  require(SUCCEEDED(sourceParent->GetUIObjectOf(fixture.frame, 1, &child, IID_IDataObject, nullptr,
          reinterpret_cast<void **>(data.GetAddressOf()))) && data,
          "create Shell data object for isolated drop");
  const auto first = point(0), second = point(1);
  constexpr DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  auto effect = allowed;
  auto &a = fixture.parent.targets[0].calls;
  auto &b = fixture.parent.targets[1].calls;
  const int before = fixture.browser.navigations;
  require(SUCCEEDED(observedDropTarget->DragEnter(data.Get(), MK_CONTROL | MK_LBUTTON, first, &effect)) &&
          effect == DROPEFFECT_COPY && a.size() == 1 && a.back().event == DropEvent::Enter &&
          a.back().data == data.Get() && a.back().keys == (MK_CONTROL | MK_LBUTTON) &&
          a.back().point.x == first.x && a.back().point.y == first.y && a.back().allowed == allowed,
          "background tab forwards original Shell object, screen coordinates, keys and effects");
  effect = allowed;
  require(SUCCEEDED(observedDropTarget->DragOver(MK_LBUTTON, first, &effect)) && effect == DROPEFFECT_MOVE &&
          a.back().event == DropEvent::Over, "same tab negotiates a changed modifier with its existing target");
  effect = allowed;
  require(SUCCEEDED(observedDropTarget->DragOver(MK_LBUTTON, second, &effect)) && effect == DROPEFFECT_MOVE &&
          a.back().event == DropEvent::Leave && !b.empty() && b.front().event == DropEvent::Enter,
          "crossing tabs leaves previous target and enters the correct active folder");
  effect = allowed;
  require(SUCCEEDED(observedDropTarget->DragOver(MK_LBUTTON, POINTL{second.x, second.y + 100}, &effect)) &&
          effect == DROPEFFECT_NONE && b.back().event == DropEvent::Leave,
          "blank strip does not accept a drop");
  effect = allowed;
  require(SUCCEEDED(observedDropTarget->DragOver(MK_LBUTTON, first, &effect)) && effect == DROPEFFECT_MOVE &&
          a.back().event != DropEvent::Leave, "re-enter the first folder in the same drag");
  effect = allowed;
  require(SUCCEEDED(observedDropTarget->Drop(data.Get(), MK_CONTROL, first, &effect)) && effect == DROPEFFECT_COPY &&
          a.back().event == DropEvent::Drop && a.back().data == data.Get() && a.back().keys == MK_CONTROL &&
          fixture.browser.navigations == before && TabCtrl_GetCurSel(control) == 1 && b.back().event == DropEvent::Leave,
          "drop goes to the background folder without switching the visible tab");
  effect = allowed;
  require(SUCCEEDED(observedDropTarget->DragEnter(data.Get(), MK_LBUTTON, first, &effect)) &&
          effect == DROPEFFECT_MOVE, "a new drag can follow a completed drop");
  require(SUCCEEDED(fixture.site->SetSite(nullptr)) && a.back().event == DropEvent::Leave,
          "detaching the tab strip leaves the native target during an active drag");
  observedDropTarget.Reset();
  band.Reset();
  dropParent = nullptr;
  fixture.browser.view.folder = destinationFolder.location();
  require(SUCCEEDED(fixture.site->SetSite(&fixture.browser)) && observedDropTarget, "attach with a real drop folder");
  require(SUCCEEDED(fixture.site.As(&band)) && SUCCEEDED(band->GetWindow(&window)), "get native drop window");
  MoveWindow(window, 0, 0, 900, 40, FALSE);
  HWND nativeTabs = GetDlgItem(window, 1);
  RECT nativeBounds{};
  require(TabCtrl_GetItemRect(nativeTabs, 0, &nativeBounds) != FALSE, "locate native drop folder tab");
  POINT nativePoint{(nativeBounds.left + nativeBounds.right) / 2, (nativeBounds.top + nativeBounds.bottom) / 2};
  ClientToScreen(nativeTabs, &nativePoint);
  PIDLIST_ABSOLUTE sourceId = nullptr;
  require(SUCCEEDED(SHParseDisplayName(sourceFile.c_str(), nullptr, &sourceId, 0, nullptr)), "parse actual source file");
  struct SourcePidl { PIDLIST_ABSOLUTE value; ~SourcePidl() { CoTaskMemFree(value); } } sourcePidl{sourceId};
  sourceParent.Reset();
  data.Reset();
  require(SUCCEEDED(SHBindToParent(sourceId, IID_PPV_ARGS(&sourceParent), &child)) && child &&
          SUCCEEDED(sourceParent->GetUIObjectOf(fixture.frame, 1, &child, IID_IDataObject, nullptr,
            reinterpret_cast<void **>(data.GetAddressOf()))) && data, "create native file drag data");
  effect = allowed;
  const POINTL nativeDrop{nativePoint.x, nativePoint.y};
  require(SUCCEEDED(observedDropTarget->DragEnter(data.Get(), MK_CONTROL | MK_LBUTTON, nativeDrop, &effect)) &&
          effect == DROPEFFECT_COPY, "actual folder accepts Ctrl-drag with copy effect");
  effect = allowed;
  require(SUCCEEDED(observedDropTarget->Drop(data.Get(), MK_CONTROL, nativeDrop, &effect)) &&
          effect == DROPEFFECT_COPY, "actual folder receives native Shell copy drop");
  const auto deadline = GetTickCount64() + 5000;
  while (GetFileAttributesW(copiedFile.c_str()) == INVALID_FILE_ATTRIBUTES && GetTickCount64() < deadline) {
    pump();
    Sleep(20);
  }
  require(GetFileAttributesW(copiedFile.c_str()) != INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(sourceFile.c_str()) != INVALID_FILE_ATTRIBUTES, "native drop copies the file into the tab folder");
  require(SUCCEEDED(fixture.site->SetSite(nullptr)), "detach native drop fixture");
  observedDropTarget.Reset();
  band.Reset();
  fixture.site.Reset();
  require(tabs::activeObjects() == 0, "detached drag releases all band objects");
}
struct RebarReentry {
  std::function<void()> onSet;
  unsigned maximizes = 0, writes = 0;
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto &self = *reinterpret_cast<RebarReentry *>(data);
    if (message == RB_MAXIMIZEBAND) ++self.maximizes;
    if (message == RB_SETBANDINFOW) ++self.writes;
    auto result = DefSubclassProc(window, message, w, l);
    if (message == RB_SETBANDINFOW && self.onSet) { auto callback = std::move(self.onSet); callback(); }
    return result;
  }
};
static void directHostTests() {
  if (!navigation::tabsSupported()) return;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_COOL_CLASSES}; InitCommonControlsEx(&controls);
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"CabinetWClass"; cls.lpfnWndProc = DefWindowProcW; RegisterClassW(&cls);
  HWND frame = CreateWindowW(cls.lpszClassName, L"Hidden direct band host", WS_OVERLAPPED, 0, 0, 900, 300, nullptr, nullptr, cls.hInstance, nullptr);
  auto makeRebar = [&] { return CreateWindowExW(0, REBARCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | RBS_VARHEIGHT | CCS_NODIVIDER | CCS_NORESIZE,
      0, 0, 900, 70, frame, nullptr, cls.hInstance, nullptr); };
  Browser browser; browser.parent = frame;
  tabs::Host host;
  DWORD enabled = 1; RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  require(!host.update(frame, &browser, nullptr), "missing rebar is retryable without changing Explorer view geometry");
  HWND rebar = makeRebar(); require(rebar != nullptr, "create real hidden test rebar");
  HWND existing = CreateWindowW(L"STATIC", L"Unrelated band", WS_CHILD | WS_VISIBLE, 0, 0, 500, 24, rebar, nullptr, cls.hInstance, nullptr);
  REBARBANDINFOW info{sizeof(info)}; info.fMask = RBBIM_CHILD | RBBIM_ID | RBBIM_CHILDSIZE | RBBIM_SIZE;
  info.hwndChild = existing; info.wID = 0x464e5400; info.cxMinChild = 200; info.cyMinChild = 24; info.cx = 800;
  require(SendMessageW(rebar, RB_INSERTBANDW, WPARAM(-1), reinterpret_cast<LPARAM>(&info)) != 0, "insert unrelated existing toolbar row");
  HWND detaching = CreateWindowW(L"STATIC", L"Detaching child", WS_CHILD | WS_VISIBLE, 0, 0, 100, 20, rebar, nullptr, cls.hInstance, nullptr);
  SetPropW(frame, tabs::windowProperty, detaching);
  require(!host.update(frame, &browser, nullptr) && IsWindow(detaching), "published child without rebar row is retryable and remains untouched");
  RemovePropW(frame, tabs::windowProperty); DestroyWindow(detaching);
  require(host.update(frame, &browser, nullptr), "software-enabled BHO hosts band without ShowBrowserBar");
  HWND tabWindow = static_cast<HWND>(GetPropW(frame, tabs::windowProperty));
  require(IsWindow(tabWindow) && GetParent(tabWindow) == rebar && (GetWindowLongPtrW(tabWindow, GWL_STYLE) & WS_VISIBLE), "hosted band visible child belongs to native rebar");
  require(SendMessageW(rebar, RB_GETBANDCOUNT, 0, 0) == 2, "host adds exactly one native row");
  for (int i = 0; i < 4; ++i) require(host.update(frame, &browser, nullptr), "repeat refresh reuses existing host");
  require(SendMessageW(rebar, RB_GETBANDCOUNT, 0, 0) == 2 && GetPropW(frame, tabs::windowProperty) == tabWindow, "updates never duplicate native row");
  SendMessageW(tabWindow, tabs::actionMessage, tabs::Duplicate, 0); pump();
  require(TabCtrl_GetItemCount(GetDlgItem(tabWindow, 1)) == 2, "direct host runs actual tab commands");
  MoveWindow(rebar, 0, 0, 700, 70, FALSE);
  require(IsWindow(tabWindow) && tabs::childBand(rebar, existing) >= 0, "resize retains tabs and unrelated row");
  RebarReentry reentry;
  require(SetWindowSubclass(rebar, RebarReentry::proc, 1, reinterpret_cast<DWORD_PTR>(&reentry)) != 0, "install isolated rebar reentry probe");
  observedVisibilityWindow = tabWindow; visibilityRequests = 0;
  {
    ControlChanges tabChanges(tabWindow);
    for (int repeat = 0; repeat < 4; ++repeat)
      require(host.update(frame, &browser, nullptr), "repeated host update remains ready");
    pump();
    require(!reentry.writes && !reentry.maximizes && !visibilityRequests && !tabChanges.geometry,
            "unchanged host updates do not relayout or reshow band");
    const HWND tabControl = GetDlgItem(tabWindow, 1), overflow = GetDlgItem(tabWindow, 3);
    const auto beforeBand = relativeRect(tabWindow, rebar), beforeControl = relativeRect(tabControl, tabWindow),
               beforeOverflow = relativeRect(overflow, tabWindow);
    RECT beforeItem{}; TabCtrl_GetItemRect(tabControl, 0, &beforeItem);
    for (int repeat = 0; repeat < 4; ++repeat)
      SendMessageW(rebar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(700, 70));
    pump();
    const auto afterBand = relativeRect(tabWindow, rebar), afterControl = relativeRect(tabControl, tabWindow),
               afterOverflow = relativeRect(overflow, tabWindow);
    RECT afterItem{}; TabCtrl_GetItemRect(tabControl, 0, &afterItem);
    // The native rebar may send SetWindowPos for every WM_SIZE, even with an
    // unchanged rectangle. Verify actual geometry and our own mutation calls.
    require(!reentry.writes && !reentry.maximizes && !visibilityRequests &&
            EqualRect(&beforeBand, &afterBand) && EqualRect(&beforeControl, &afterControl) &&
            EqualRect(&beforeOverflow, &afterOverflow) && EqualRect(&beforeItem, &afterItem),
            "same-size native rebar messages preserve band, tab and overflow geometry without host mutations");
  }
  observedVisibilityWindow = nullptr;
  bool recreated = false;
  unsigned newGenerationMaximizes = 0;
  reentry.onSet = [&] {
    host.clear(); recreated = host.update(frame, &browser, nullptr);
    newGenerationMaximizes = reentry.maximizes;
  };
  const auto oldDpi = simulatedDpi;
  simulatedDpi = GetDpiForWindow(rebar) == 144 ? 192 : 144;
  SendMessageW(rebar, WM_DPICHANGED_AFTERPARENT, 0, 0);
  require(recreated && reentry.maximizes == newGenerationMaximizes,
          "DPI resize interrupted by new host cannot maximize again using a stale row index");
  simulatedDpi = oldDpi;
  SendMessageW(rebar, WM_DPICHANGED_AFTERPARENT, 0, 0);
  RemoveWindowSubclass(rebar, RebarReentry::proc, 1);
  tabWindow = static_cast<HWND>(GetPropW(frame, tabs::windowProperty));
  require(IsWindow(tabWindow) && tabs::childBand(rebar, tabWindow) >= 0 && tabs::childBand(rebar, existing) >= 0 && SendMessageW(rebar, RB_GETBANDCOUNT, 0, 0) == 2,
      "reentrant resize leaves only new host and unrelated row");
  enabled = 0; RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  require(host.update(frame, &browser, nullptr), "software disable succeeds");
  require(!IsWindow(tabWindow) && !GetPropW(frame, tabs::windowProperty) && SendMessageW(rebar, RB_GETBANDCOUNT, 0, 0) == 1 && tabs::childBand(rebar, existing) >= 0,
      "disable removes only own row and destroys tab window");
  enabled = 1; RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled));
  require(host.update(frame, &browser, nullptr), "software re-enable attaches again");
  DestroyWindow(rebar); pump();
  require(!GetPropW(frame, tabs::windowProperty) && tabs::activeObjects() == 0, "rebar destruction releases tab host");
  rebar = makeRebar(); require(host.update(frame, &browser, nullptr), "recreated toolbar can host tabs again");
  host.clear(); host.clear();
  require(SendMessageW(rebar, RB_GETBANDCOUNT, 0, 0) == 0 && tabs::activeObjects() == 0, "repeat clear is harmless");
  HWND second = makeRebar();
  require(!host.update(frame, &browser, nullptr), "ambiguous rebar selection does not alter any toolbar");
  MoveWindow(second, 0, 0, 900, 0, FALSE);
  require(tabs::findRebar(frame, &browser, GetCurrentThreadId()) == rebar, "zero-height legacy toolbar does not hide unique visible navigation rebar");
  MoveWindow(second, 0, 0, 900, 70, FALSE);
  WNDCLASSW addressClass = cls, travelClass = cls;
  addressClass.lpszClassName = L"Address Band Root"; travelClass.lpszClassName = L"TravelBand";
  require(RegisterClassW(&addressClass) && RegisterClassW(&travelClass), "register disposable Explorer navigation child classes");
  auto marker = [&](HWND parent, LPCWSTR name) { return CreateWindowW(name, L"", WS_CHILD | WS_VISIBLE, 0, 0, 20, 20, parent, nullptr, cls.hInstance, nullptr); };
  HWND address = marker(rebar, addressClass.lpszClassName), travel = marker(rebar, travelClass.lpszClassName);
  require(address && travel && tabs::findRebar(frame, &browser, GetCurrentThreadId()) == rebar, "address and travel children identify navigation rebar among two nonempty rebars");
  require(host.update(frame, &browser, nullptr) && GetParent(static_cast<HWND>(GetPropW(frame, tabs::windowProperty))) == rebar && SendMessageW(second, RB_GETBANDCOUNT, 0, 0) == 0,
      "dual-rebar Explorer receives tabs only in identified navigation row");
  host.clear();
  HWND secondAddress = marker(second, addressClass.lpszClassName), secondTravel = marker(second, travelClass.lpszClassName);
  require(secondAddress && secondTravel && !tabs::findRebar(frame, &browser, GetCurrentThreadId()), "two matching navigation rebars remain ambiguous");
  DestroyWindow(second);
  require(host.update(frame, &browser, nullptr), "unambiguous toolbar recovers after ambiguity removed");
  DestroyWindow(frame); pump(); host.clear();
  require(tabs::activeObjects() == 0 && browser.refs == 1 && browser.view.refs == 1, "frame destruction releases adapter and browser references");
  UnregisterClassW(cls.lpszClassName, cls.hInstance);
  UnregisterClassW(addressClass.lpszClassName, cls.hInstance); UnregisterClassW(travelClass.lpszClassName, cls.hInstance);
}
static void packetTests() {
  using namespace tabs::routing;
  Packet original{73, known(FOLDERID_Profile).id, {{4, 0, 1, 2, 0, 0}, {4, 0, 3, 4, 0, 0}}};
  auto bytes = encode(original);
  COPYDATASTRUCT data{importData, DWORD(bytes.size()), bytes.data()}; Packet decoded;
  require(!bytes.empty() && decode(&data, decoded) && decoded.token == original.token &&
          decoded.folder == original.folder && decoded.selected == original.selected, "routing packet round trips folder and selected child PIDLs");
  for (DWORD length = 0; length < bytes.size(); ++length) {
    data.cbData = length; require(!decode(&data, decoded), "every truncated routing packet is rejected");
  }
  data.cbData = DWORD(bytes.size()); data.dwData = 0;
  require(!decode(&data, decoded), "wrong copy-data protocol is rejected");
  data.dwData = importData;
  auto invalid = bytes; PacketHeader header{}; memcpy(&header, invalid.data(), sizeof(header));
  header.selectedCount = DWORD(maxSelection + 1); memcpy(invalid.data(), &header, sizeof(header)); data.lpData = invalid.data();
  require(!decode(&data, decoded), "packet cannot exceed bounded selection count");
  invalid = bytes; invalid[sizeof(PacketHeader)] = 1; invalid[sizeof(PacketHeader) + 1] = 0; data.lpData = invalid.data();
  require(!decode(&data, decoded), "malformed folder item length is rejected");
  invalid = bytes; invalid.back() = 1; data.lpData = invalid.data();
  require(!decode(&data, decoded), "unterminated selected child PIDL is rejected");
  invalid = bytes; invalid.push_back(0); data.lpData = invalid.data(); data.cbData = DWORD(invalid.size());
  require(!decode(&data, decoded), "trailing packet data is rejected");
  require(decoded.token == original.token && decoded.selected == original.selected, "rejected packet never replaces the last decoded value");
  original.token = 0; require(encode(original).empty(), "zero transfer token is rejected"); original.token = 73;
  original.selected.resize(maxSelection + 1); require(encode(original).empty(), "encoder bounds selection allocation");
}
struct RoutingHost {
  Browser browser;
  ComPtr<IObjectWithSite> site;
  ComPtr<IDeskBand2> band;
  HWND frame = nullptr, window = nullptr, control = nullptr;
  unsigned closeRequests = 0;
  ULONG_PTR receivedToken = 0;
  static LRESULT CALLBACK frameProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
    auto self = reinterpret_cast<RoutingHost *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && message == WM_CLOSE) { ++self->closeRequests; return 0; }
    return DefWindowProcW(h, message, w, l);
  }
  static LRESULT CALLBACK observe(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR value) {
    auto self = reinterpret_cast<RoutingHost *>(value);
    if (message == WM_COPYDATA) {
      tabs::routing::Packet packet;
      if (tabs::routing::decode(reinterpret_cast<const COPYDATASTRUCT *>(l), packet)) self->receivedToken = packet.token;
    }
    return DefSubclassProc(h, message, w, l);
  }
  explicit RoutingHost(tabs::Location folder) {
    frame = CreateWindowW(L"CabinetWClass", L"Hidden tab routing fixture", WS_OVERLAPPED,
        0, 0, 1100, 200, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    require(frame != nullptr, "create isolated routing frame");
    browser.parent = frame; browser.view.folder = std::move(folder);
    require(SUCCEEDED(tabs::create(IID_PPV_ARGS(&site))) && SUCCEEDED(site->SetSite(&browser)) &&
            SUCCEEDED(site.As(&band)) && SUCCEEDED(band->GetWindow(&window)), "attach routing fixture band");
    MoveWindow(window, 0, 0, 1100, 40, FALSE); control = GetDlgItem(window, 1);
    require(SetWindowSubclass(window, observe, 81, reinterpret_cast<DWORD_PTR>(this)) != FALSE, "observe copied routing token");
    pump();
  }
  ~RoutingHost() {
    if (IsWindow(window)) RemoveWindowSubclass(window, observe, 81);
    if (site) site->SetSite(nullptr); band.Reset(); site.Reset();
    if (IsWindow(frame)) DestroyWindow(frame); pump();
  }
  int count() const { return TabCtrl_GetItemCount(control); }
  LRESULT route(RoutingHost &destination, HWND helper) {
    return SendMessageW(window, tabs::routing::routeMessage, reinterpret_cast<WPARAM>(destination.window), reinterpret_cast<LPARAM>(helper));
  }
  void finish(tabs::Location folder) {
    browser.view.folder = std::move(folder); SendMessageW(window, tabs::changedMessage, 0, 0); pump();
  }
};
static void routingTests() {
  if (!navigation::tabsSupported()) return;
  auto preference = [](bool enabled) {
    DWORD value = enabled ? 1 : 0;
    require(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerOpenInNewTab", REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS,
            "set isolated new-tab routing preference");
  };
  DWORD enabled = 1;
  require(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"ExplorerTabsEnabled", REG_DWORD, &enabled, sizeof(enabled)) == ERROR_SUCCESS,
          "enable isolated tab fixture");
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"CabinetWClass"; cls.lpfnWndProc = RoutingHost::frameProc;
  require(RegisterClassW(&cls) != 0, "register routing fixture class");
  cls.lpszClassName = tabs::routing::helperClass; cls.lpfnWndProc = DefWindowProcW;
  require(RegisterClassW(&cls) != 0, "register inert helper fixture class");
  HWND helper = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
  testOwnedHelper = helper;
  require(helper && tabs::routing::helperWindow() == helper, "routing requests use only the test-owned inert helper");
  auto first = known(FOLDERID_Windows), second = known(FOLDERID_Profile), third = known(FOLDERID_ComputerFolder);
  preference(false);
  {
    RoutingHost host(first);
    SendMessageW(host.window, tabs::actionMessage, tabs::Duplicate, 0); pump();
    host.finish(second); const auto before = recordedLaunches.size(); const auto navigations = host.browser.navigations;
    SendMessageW(host.window, tabs::actionMessage, tabs::NewWindow, 0); pump();
    require(recordedLaunches.size() == before + 1 && recordedLaunches.back().verb == L"opennewwindow" &&
            (recordedLaunches.back().mask & SEE_MASK_IDLIST) && recordedLaunches.back().folder == first.id,
            "new-window command launches the clicked inactive tab through its absolute PIDL");
    require(host.count() == 2 && host.browser.view.folder == second && host.browser.navigations == navigations && !host.closeRequests,
            "explicit new window preserves original tabs, active folder and source window");
  }
  preference(true);
  {
    RoutingHost source(first), destination(second);
    source.browser.view.onGetFolder = [&] { SendMessageW(source.window, tabs::changedMessage, 0, 0); };
    require(source.route(destination, helper) == 0 && !source.closeRequests && destination.count() == 1,
            "startup completion during capture keeps a held window eligible for retry");
    require(source.route(destination, helper) == 1, "settled capture accepts the existing pending source"); pump();
    require(source.closeRequests == 1 && destination.count() == 2,
            "capture retry completes once without revealing or duplicating the source");
  }
  {
    RoutingHost source(first), destination(first);
    destination.browser.view.selected = {{4, 0, 1, 2, 0, 0}};
    require(source.route(destination, helper) == 1, "accept same-folder route with empty source selection"); pump();
    require(destination.count() == 1 && TabCtrl_GetCurSel(destination.control) == 0 &&
            destination.browser.navigations == 0 && destination.browser.view.selected.empty() && source.closeRequests == 1,
            "same-folder import activates existing tab and clears selection without redundant navigation or duplicate tab");
  }
  {
    RoutingHost source(first), destination(first);
    SendMessageW(destination.window, tabs::actionMessage, tabs::Duplicate, 0); pump();
    destination.finish(second);
    source.browser.view.selected = {{4, 0, 1, 2, 0, 0}};
    const int beforeNavigations = destination.browser.navigations;
    require(source.route(destination, helper) == 1, "accept route for inactive existing tab"); pump();
    require(destination.count() == 2 && TabCtrl_GetCurSel(destination.control) == 0 &&
            destination.browser.navigations == beforeNavigations + 1 &&
            destination.browser.view.folder == first &&
            destination.browser.view.selected == source.browser.view.selected && source.closeRequests == 1,
            "routing existing inactive folder activates that tab and restores selection without creating a duplicate");
  }
  {
    RoutingHost source(first), destination(first);
    SendMessageW(destination.window, tabs::actionMessage, tabs::Duplicate, 0); pump();
    require(destination.count() == 2 && TabCtrl_GetCurSel(destination.control) == 1,
            "tab-bar duplicate still creates a second tab when new-tab routing is enabled");
    const int beforeNavigations = destination.browser.navigations;
    require(source.route(destination, helper) == 1, "accept route when duplicate tab is active"); pump();
    require(destination.count() == 2 && TabCtrl_GetCurSel(destination.control) == 1 &&
            destination.browser.navigations == beforeNavigations && source.closeRequests == 1,
            "routing folder on already active duplicate retains that tab without opening a third tab");
  }
  {
    RoutingHost existingSource(first), newSource(third), destination(first);
    const UINT dpi = GetDpiForWindow(destination.window);
    MoveWindow(destination.window, 0, 0, MulDiv(8000, dpi ? dpi : 96, 96), 40, FALSE);
    SendMessageW(destination.window, tabs::actionMessage, tabs::Duplicate, 0); pump();
    destination.finish(second);
    for (size_t i = 2; i < tabs::Model::limit; ++i) {
      SendMessageW(destination.window, tabs::actionMessage, tabs::Duplicate, LPARAM(i - 1)); pump();
    }
    require(destination.count() == int(tabs::Model::limit) && TabCtrl_GetCurSel(destination.control) == int(tabs::Model::limit) - 1,
            "destination reaches max tab limit with target folder at index zero");
    require(existingSource.route(destination, helper) == 1, "allow route for already-open folder at tab limit"); pump();
    require(destination.count() == int(tabs::Model::limit) && TabCtrl_GetCurSel(destination.control) == 0 &&
            destination.browser.view.folder == first && existingSource.closeRequests == 1,
            "already-open folder activates existing tab even when at tab limit");
    require(newSource.route(destination, helper) == 2 && destination.count() == int(tabs::Model::limit) && !newSource.closeRequests,
            "unopened folder still respects max tab limit");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    source.browser.view.selected = {{4, 0, 1, 2, 0, 0}, {4, 0, 3, 4, 0, 0}};
    require(source.route(destination, helper) == 1 && !source.closeRequests && destination.browser.navigations == 0,
            "accepting copied request neither navigates nor closes a source synchronously");
    pump(); require(destination.count() == 2 && destination.browser.navigations == 1 && !source.closeRequests,
                    "queued import starts a new tab but source remains until the directory loads");
    SendMessageW(source.window, tabs::routing::completedMessage, reinterpret_cast<WPARAM>(source.window), LPARAM(destination.receivedToken));
    SendMessageW(source.window, tabs::routing::completedMessage, reinterpret_cast<WPARAM>(destination.window), LPARAM(destination.receivedToken + 1));
    require(!source.closeRequests, "wrong sender and wrong token cannot close a source window");
    destination.finish(first);
    require(source.closeRequests == 1 && destination.browser.view.selected == source.browser.view.selected &&
            destination.browser.view.selectionFlags.size() == 2 &&
            (destination.browser.view.selectionFlags[0] & SVSI_DESELECTOTHERS) &&
            !(destination.browser.view.selectionFlags[1] & SVSI_DESELECTOTHERS),
            "confirmed folder and restored selection acknowledge exactly one source close");
    SendMessageW(source.window, tabs::routing::completedMessage, reinterpret_cast<WPARAM>(destination.window), LPARAM(destination.receivedToken));
    require(source.closeRequests == 1, "duplicate completion never closes twice");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.outcome = E_ACCESSDENIED;
    require(source.route(destination, helper) == 1, "navigation failure occurs after request acceptance"); pump();
    require(!source.closeRequests && destination.count() == 1 && destination.browser.view.folder == second,
            "failed destination navigation preserves source and original destination tab");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    SendMessageW(destination.window, tabs::actionMessage, tabs::New, 0); pump();
    require(source.route(destination, helper) == 0 && !source.closeRequests, "busy destination requests retry without closing source");
    require(source.route(destination, nullptr) == 2, "non-helper route requests are rejected");
  }
  {
    RoutingHost source(first), destination(second);
    BYTE invalid[] = {1, 2, 3}; COPYDATASTRUCT data{tabs::routing::importData, sizeof(invalid), invalid};
    require(!SendMessageW(destination.window, WM_COPYDATA, reinterpret_cast<WPARAM>(source.window), reinterpret_cast<LPARAM>(&data)),
            "malformed transfer cannot create a destination tab");
    preference(false);
    require(source.route(destination, helper) == 2 && destination.count() == 1 && !source.closeRequests,
            "disabled global preference rejects transfer and preserves windows"); preference(true);
  }
  for (int change = 0; change < 4; ++change) {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    if (change == 3) source.browser.view.selected = {{4, 0, 1, 2, 0, 0}};
    require(source.route(destination, helper) == 1, "prepare source-preservation transfer"); pump();
    if (change == 0) source.finish(third);
    if (change == 1) source.browser.view.selected = {{4, 0, 5, 6, 0, 0}};
    if (change == 2) preference(false);
    if (change == 3) destination.browser.view.selectOutcome = E_ACCESSDENIED;
    destination.finish(first);
    require(!source.closeRequests, "changed source folder, changed selection, disabled setting or failed selection restoration preserves source");
    if (change == 2) preference(true);
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    require(source.route(destination, helper) == 1, "prepare asynchronous failure transfer"); pump();
    SendMessageW(destination.window, tabs::failedMessage, 0, 0); pump();
    require(!source.closeRequests && destination.count() == 1, "asynchronous failed directory load rolls back destination and preserves source");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    require(source.route(destination, helper) == 1, "prepare expiring outbound transfer"); pump();
    const auto token = destination.receivedToken;
    simulatedElapsed += 20001; SendMessageW(source.window, WM_TIMER, 2, 0);
    SendMessageW(source.window, tabs::routing::completedMessage, reinterpret_cast<WPARAM>(destination.window), LPARAM(token));
    require(!source.closeRequests && source.route(destination, helper) == 0,
            "outbound expiry ignores late acknowledgment and releases source for a busy-destination retry");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    require(source.route(destination, helper) == 1, "prepare destination-detach transfer"); pump();
    const auto token = destination.receivedToken; const auto oldDestination = destination.window;
    destination.site->SetSite(nullptr);
    SendMessageW(source.window, tabs::routing::completedMessage, reinterpret_cast<WPARAM>(oldDestination), LPARAM(token));
    require(!source.closeRequests, "detaching pending destination preserves source even if stale acknowledgment arrives");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    require(source.route(destination, helper) == 1, "prepare reentrant source-query transfer"); pump();
    source.browser.view.onGetFolder = [&] {
      source.browser.view.folder = third; SendMessageW(source.window, tabs::changedMessage, 0, 0);
    };
    destination.finish(first);
    SendMessageW(source.window, WM_TIMER, 4, 0); pump();
    require(!source.closeRequests && source.browser.view.folder == third,
            "source navigation pumped by final folder query invalidates stale close authorization");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    source.browser.view.selected = {{4, 0, 1, 2, 0, 0}};
    require(source.route(destination, helper) == 1, "prepare reentrant selection-restore transfer"); pump();
    destination.browser.view.onSelectItem = [&] {
      destination.browser.view.folder = third; SendMessageW(destination.window, tabs::changedMessage, 0, 0);
    };
    destination.finish(first);
    SendMessageW(destination.window, WM_TIMER, 3, 0); pump();
    require(!source.closeRequests && destination.browser.view.folder == third,
            "destination navigation pumped by selection restoration prevents success acknowledgment");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    require(source.route(destination, helper) == 1, "prepare benign source completion reentrancy"); pump();
    source.browser.view.onGetFolder = [&] { SendMessageW(source.window, tabs::changedMessage, 0, 0); };
    destination.finish(first);
    require(!source.closeRequests, "source defers close when a completion pumps its final snapshot");
    SendMessageW(source.window, WM_TIMER, 4, 0); pump();
    require(source.closeRequests == 1 && destination.count() == 2 && destination.browser.navigations == 1,
            "same-folder completion retry closes source without creating a duplicate tab");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    source.browser.view.selected = {{4, 0, 1, 2, 0, 0}};
    require(source.route(destination, helper) == 1, "prepare benign destination completion reentrancy"); pump();
    destination.browser.view.onSelectItem = [&] { SendMessageW(destination.window, tabs::changedMessage, 0, 0); };
    destination.finish(first);
    require(!source.closeRequests, "destination defers acknowledgment during nested completion");
    SendMessageW(destination.window, WM_TIMER, 3, 0); pump();
    require(source.closeRequests == 1 && destination.count() == 2 && destination.browser.navigations == 1 &&
            destination.browser.view.selected == source.browser.view.selected,
            "same-folder completion retry restores selection then acknowledges once");
  }
  {
    RoutingHost source(first), destination(second); destination.browser.complete = false;
    source.browser.view.selected = {{4, 0, 1, 2, 0, 0}};
    require(source.route(destination, helper) == 1, "prepare transient selection readiness failure"); pump();
    destination.browser.view.selectOutcome = E_FAIL;
    destination.finish(first);
    require(!source.closeRequests, "unready selection does not close source");
    destination.browser.view.selectOutcome = S_OK;
    SendMessageW(destination.window, WM_TIMER, 3, 0); pump();
    require(source.closeRequests == 1 && destination.count() == 2 && destination.browser.navigations == 1,
            "selection readiness retry completes existing import without opening another tab");
  }
  preference(false); testOwnedHelper = nullptr; DestroyWindow(helper);
  UnregisterClassW(tabs::routing::helperClass, cls.hInstance); UnregisterClassW(L"CabinetWClass", cls.hInstance);
  require(tabs::activeObjects() == 0, "routing fixtures release all bands without touching Explorer");
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  GUID token{}; CoCreateGuid(&token); wchar_t text[40]{}; StringFromGUID2(token, text, ARRAYSIZE(text));
  auto key = std::wstring(L"Software\\ShibaKitTabsTests\\") + text;
  HKEY root = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) || RegOverridePredefKey(HKEY_CURRENT_USER, root)) return 2;
  int result = 0;
  try { models(); bandTests(); stableLayoutTests(); rollbackViewportTests(); tabInputTests(); tabDropTests(); directHostTests(); packetTests(); routingTests(); std::cout << "PASS " << checks << " tab assertions: independent folders, stable DPI layout, blank-area duplication, navigation rollback, reentrant detach, folder drop forwarding, direct rebar hosting, confirmed new-tab routing\n"; }
  catch (const char *message) { std::cerr << "FAIL " << checks << ": " << message << "\n"; result = 1; }
  catch (...) { std::cerr << "FAIL unexpected exception\n"; result = 1; }
  RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(root); RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  CoUninitialize(); return result;
}
