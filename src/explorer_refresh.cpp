#include "explorer_refresh.h"
#include <ole2.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <wrl/client.h>
#include <filesystem>
#include <set>
#include <algorithm>

using Microsoft::WRL::ComPtr;
namespace fn {
namespace refresh_detail {
namespace fs = std::filesystem;

static std::wstring shellPath(const fs::path &path) {
  auto text = path.native();
  // Shell parsing uses normal filesystem paths, including for the equivalent
  // extended-length spelling accepted by the note writer.
  if (text.starts_with(L"\\\\?\\UNC\\"))
    return L"\\\\" + text.substr(8);
  if (text.starts_with(L"\\\\?\\") && text.size() > 6 && text[5] == L':')
    return text.substr(4);
  return text;
}

std::vector<Pidl> affectedParents(const std::vector<std::wstring> &changedFolders) {
  std::vector<Pidl> result;
  std::set<std::pair<bool, std::wstring>> paths;
  for (const auto &text : changedFolders) {
    if (text.empty())
      continue;
    std::error_code error;
    auto folder = fs::absolute(fs::path(text), error).lexically_normal();
    if (error || folder.empty())
      continue;
    while (folder.filename().empty() && folder.parent_path() != folder)
      folder = folder.parent_path();
    const auto parent = folder.parent_path();
    if (parent.empty())
      continue;
    bool root = parent == folder;
    auto parsingPath = shellPath(root ? folder : parent);
    // A large batch in one directory resolves that parent only once.
    if (!paths.emplace(root, parsingPath).second)
      continue;
    PIDLIST_ABSOLUTE raw = nullptr;
    if (FAILED(SHParseDisplayName(parsingPath.c_str(), nullptr, &raw, 0, nullptr)) || !raw) {
      CoTaskMemFree(raw);
      continue;
    }
    Pidl id(raw);
    // A volume's Shell parent is "This PC", not the volume itself.
    if (root && !ILRemoveLastID(id.get()))
      continue;
    if (std::none_of(result.begin(), result.end(), [&](const Pidl &known) {
          return ILIsEqual(known.get(), id.get()) != FALSE;
        }))
      result.push_back(std::move(id));
  }
  return result;
}

HRESULT refreshMatchingView(IShellView *view,
                            std::span<const PCIDLIST_ABSOLUTE> parents) noexcept {
  if (!view)
    return E_POINTER;
  if (parents.empty())
    return S_FALSE;
  try {
    ComPtr<IFolderView> folderView;
    HRESULT hr = view->QueryInterface(IID_PPV_ARGS(&folderView));
    if (FAILED(hr) || !folderView)
      return FAILED(hr) ? hr : E_NOINTERFACE;
    ComPtr<IPersistFolder2> folder;
    hr = folderView->GetFolder(IID_PPV_ARGS(&folder));
    if (FAILED(hr) || !folder)
      return FAILED(hr) ? hr : E_NOINTERFACE;
    PIDLIST_ABSOLUTE raw = nullptr;
    hr = folder->GetCurFolder(&raw);
    Pidl current(raw);
    if (hr != S_OK || !current)
      return FAILED(hr) ? hr : S_FALSE;
    if (std::none_of(parents.begin(), parents.end(), [&](PCIDLIST_ABSOLUTE parent) {
          return parent && ILIsEqual(parent, current.get());
        }))
      return S_FALSE;
    // Documented equivalent of Explorer's F5. This does not activate a window,
    // navigate it, or change any desktop.ini content.
    return view->Refresh();
  } catch (...) {
    return E_FAIL;
  }
}
} // namespace refresh_detail

static bool explorerView(HWND view) {
  HWND frame = GetAncestor(view, GA_ROOT);
  wchar_t className[64]{};
  if (!frame || !GetClassNameW(frame, className, ARRAYSIZE(className)) ||
      (wcscmp(className, L"CabinetWClass") && wcscmp(className, L"ExploreWClass")))
    return false;
  DWORD processId = 0;
  GetWindowThreadProcessId(frame, &processId);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
  if (!process)
    return false;
  wchar_t image[32768]{}, windows[MAX_PATH]{};
  DWORD size = ARRAYSIZE(image);
  BOOL read = QueryFullProcessImageNameW(process, 0, image, &size);
  CloseHandle(process);
  UINT windowsLength = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
  if (!read || !windowsLength || windowsLength >= ARRAYSIZE(windows))
    return false;
  return _wcsicmp(image, (std::wstring(windows) + L"\\explorer.exe").c_str()) == 0;
}

void refreshOpenViews(const std::vector<std::wstring> &changedFolders) noexcept {
  try {
    auto targets = refresh_detail::affectedParents(changedFolders);
    if (targets.empty())
      return;
    std::vector<PCIDLIST_ABSOLUTE> parents;
    for (const auto &target : targets)
      parents.push_back(target.get());
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&windows))) || !windows)
      return;
    long count = 0;
    if (FAILED(windows->get_Count(&count)))
      return;
    std::set<HWND> seenViews;
    const ULONGLONG began = GetTickCount64();
    for (long index = 0; index < count; ++index) {
      // Stop starting additional work in a large or busy Explorer session.
      // Individual COM calls still depend on the responsiveness of Explorer.
      if (GetTickCount64() - began > 2000)
        break;
      VARIANT item{};
      item.vt = VT_I4;
      item.lVal = index;
      ComPtr<IDispatch> dispatch;
      if (FAILED(windows->Item(item, &dispatch)) || !dispatch)
        continue;
      ComPtr<IServiceProvider> services;
      if (FAILED(dispatch.As(&services)))
        continue;
      ComPtr<IShellBrowser> browser;
      if (FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser)
        continue;
      ComPtr<IShellView> view;
      if (FAILED(browser->QueryActiveShellView(&view)) || !view)
        continue;
      HWND handle = nullptr;
      if (FAILED(view->GetWindow(&handle)) || !handle || !explorerView(handle) ||
          !seenViews.insert(handle).second)
        continue;
      refresh_detail::refreshMatchingView(view.Get(), parents);
    }
  } catch (...) {
    // Metadata is already safely committed. A closed or inaccessible Explorer
    // window must not turn that successful operation into a save failure.
  }
}
} // namespace fn
