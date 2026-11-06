#include "../src/explorer_refresh.h"
#include <shlobj.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value, const char *message) {
  ++checks;
  if (!value)
    throw std::runtime_error(message);
}
struct FakeView final : IShellView, IFolderView, IPersistFolder2 {
  ULONG refs = 1;
  PCIDLIST_ABSOLUTE current = nullptr;
  bool supportsFolderView = true;
  HRESULT folderResult = S_OK, currentResult = S_OK, refreshResult = S_OK;
  unsigned refreshes = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IShellView || iid == IID_IOleWindow)
      *result = static_cast<IShellView *>(this);
    else if (iid == IID_IFolderView && supportsFolderView)
      *result = static_cast<IFolderView *>(this);
    else if (iid == IID_IPersist || iid == IID_IPersistFolder || iid == IID_IPersistFolder2)
      *result = static_cast<IPersistFolder2 *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { return --refs; }
  HRESULT STDMETHODCALLTYPE GetWindow(HWND *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE UIActivate(UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Refresh() override { ++refreshes; return refreshResult; }
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
  HRESULT STDMETHODCALLTYPE GetCurrentViewMode(UINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetCurrentViewMode(UINT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFolder(REFIID iid, void **result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    return FAILED(folderResult) ? folderResult : QueryInterface(iid, result);
  }
  HRESULT STDMETHODCALLTYPE Item(int, PITEMID_CHILD *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ItemCount(UINT, int *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Items(UINT, REFIID, void **) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSelectionMarkedItem(int *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetFocusedItem(int *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetItemPosition(PCUITEMID_CHILD, POINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetSpacing(POINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDefaultSpacing(POINT *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetAutoArrange() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SelectItem(int, DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SelectAndPositionItems(UINT, PCUITEMID_CHILD_ARRAY, POINT *, DWORD) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetClassID(CLSID *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurFolder(PIDLIST_ABSOLUTE *result) override {
    if (!result) return E_POINTER;
    *result = SUCCEEDED(currentResult) && current ? ILCloneFull(current) : nullptr;
    return currentResult;
  }
};

void contracts() {
  namespace fs = std::filesystem;
  using namespace fn::refresh_detail;
  const auto root = fs::temp_directory_path() /
                    (L"FolderNote-RefreshTests-" + std::to_wstring(GetCurrentProcessId()));
  fs::create_directories(root / L"Other");
  auto targets = affectedParents({(root / L"child").native()});
  auto unrelated = affectedParents({(root / L"Other" / L"child").native()});
  check(targets.size() == 1 && unrelated.size() == 1, "resolve parent identities");
  std::vector<PCIDLIST_ABSOLUTE> parents{targets[0].get()};
  FakeView view;
  view.current = targets[0].get();
  check(refreshMatchingView(nullptr, parents) == E_POINTER, "null view rejected");
  check(refreshMatchingView(&view, {}) == S_FALSE && !view.refreshes, "empty batch ignored");
  check(refreshMatchingView(&view, parents) == S_OK && view.refreshes == 1, "matching view refreshed");
  check(view.refs == 1, "COM references released");
  parents.push_back(targets[0].get());
  check(refreshMatchingView(&view, parents) == S_OK && view.refreshes == 2, "duplicate target refreshed once");
  view.current = unrelated[0].get();
  check(refreshMatchingView(&view, parents) == S_FALSE && view.refreshes == 2, "unrelated directory not refreshed");
  view.current = nullptr;
  check(refreshMatchingView(&view, parents) == S_FALSE && view.refreshes == 2, "uninitialized view ignored");
  view.current = targets[0].get();
  view.currentResult = E_ACCESSDENIED;
  check(refreshMatchingView(&view, parents) == E_ACCESSDENIED && view.refreshes == 2,
        "folder identity failure does not refresh");
  view.currentResult = S_FALSE;
  check(refreshMatchingView(&view, parents) == S_FALSE && view.refreshes == 2, "S_FALSE identity skipped");
  view.currentResult = S_OK;
  view.folderResult = E_FAIL;
  check(refreshMatchingView(&view, parents) == E_FAIL && view.refreshes == 2, "GetFolder failure skipped");
  view.folderResult = S_OK;
  view.supportsFolderView = false;
  check(refreshMatchingView(&view, parents) == E_NOINTERFACE && view.refreshes == 2,
        "unsupported view skipped");
  view.supportsFolderView = true;
  view.refreshResult = RPC_E_DISCONNECTED;
  check(refreshMatchingView(&view, parents) == RPC_E_DISCONNECTED && view.refreshes == 3,
        "closed view failure returned");
  check(view.refs == 1, "all failure paths release COM references");

  std::vector<std::wstring> batch;
  for (int i = 0; i < 1000; ++i)
    batch.push_back((root / std::to_wstring(i)).native());
  check(affectedParents(batch).size() == 1, "1000 siblings resolve to one parent");
  batch.push_back((root / L"Other" / L"child").native());
  check(affectedParents(batch).size() == 2, "different parents preserved");
  auto variants = affectedParents({(root / L"child").native(),
                                  (root / L"child").native() + L"\\",
                                  (root / L"temporary" / L".." / L"child").native(),
                                  L"\\\\?\\" + (root / L"child").native()});
  check(variants.size() == 1 && ILIsEqual(variants[0].get(), targets[0].get()),
        "trailing slash dot segments and extended spelling share Shell parent");
  const auto previousDirectory = fs::current_path();
  fs::current_path(root);
  auto relativeTargets = affectedParents({L"child"});
  fs::current_path(previousDirectory);
  check(relativeTargets.size() == 1 && ILIsEqual(relativeTargets[0].get(), targets[0].get()),
        "relative path parent resolved");
  auto drive = root.root_path();
  check(affectedParents({drive.native(), (drive / L"child").native()}).size() == 2,
        "volume Shell parent and volume contents are distinct");
  check(affectedParents({L"", (root / L"Missing" / L"child").native()}).empty(),
        "empty and missing parents ignored");
  // Empty input cannot enumerate or alter the user's actual Explorer windows.
  fn::refreshOpenViews({});
}
} // namespace
int main() {
  HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(init)) return 1;
  int result = 0;
  try {
    contracts();
    std::cout << "PASS " << checks << " targeted view refresh and parent coalescing checks\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL " << checks << ": " << error.what() << "\n";
    result = 1;
  }
  CoUninitialize();
  return result;
}
