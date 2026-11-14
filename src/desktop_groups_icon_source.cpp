#include "desktop_groups_icon_source.h"
#include "quick_folders.h"
#include <commctrl.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <memory>
#include <utility>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;

ComPtr<IShellView> desktopView() {
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return {};
  VARIANT location{}, root{};
  location.vt = VT_I4;
  location.lVal = CSIDL_DESKTOP;
  LONG handle = 0;
  ComPtr<IDispatch> dispatch;
  ComPtr<IServiceProvider> services;
  ComPtr<IShellBrowser> browser;
  ComPtr<IShellView> view;
  if (windows->FindWindowSW(&location, &root, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, &dispatch) != S_OK ||
      !dispatch || FAILED(dispatch.As(&services)) ||
      FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser ||
      FAILED(browser->QueryActiveShellView(&view))) return {};
  return view;
}

std::wstring desktopPath(IShellFolder *desktop, PCUITEMID_CHILD entry) {
  ComPtr<IShellItem> item;
  if (FAILED(SHCreateItemWithParent(nullptr, desktop, entry, IID_PPV_ARGS(&item)))) return {};
  PWSTR raw = nullptr;
  const auto result = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
  if (FAILED(result) || !path) return {};
  auto normalized = quick::normalize(path.get());
  if (!normalized.empty()) return normalized;
  return path.get();
}

template <typename Visit>
bool forEachIcon(IEnumIDList *entries, Visit visit) {
  if (!entries) return true;
  for (;;) {
    PITEMID_CHILD raw = nullptr;
    const auto result = entries->Next(1, &raw, nullptr);
    const std::unique_ptr<void, decltype(&CoTaskMemFree)> entry(reinterpret_cast<void *>(raw), CoTaskMemFree);
    if (result != S_OK || !entry) return result == S_FALSE;
    visit(reinterpret_cast<PCUITEMID_CHILD>(entry.get()));
  }
}
}

std::optional<DesktopIconSnapshot> desktopIconPositions() {
  const auto view = desktopView();
  HWND host = nullptr;
  ComPtr<IFolderView> folderView;
  ComPtr<IShellFolder> desktop;
  ComPtr<IEnumIDList> entries;
  if (!view || FAILED(view->GetWindow(&host)) || !host ||
      !quick::isDesktop(GetAncestor(host, GA_ROOT)) || FAILED(view.As(&folderView)) ||
      FAILED(folderView->GetFolder(IID_PPV_ARGS(&desktop))) || !desktop ||
      FAILED(folderView->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&entries)))) return std::nullopt;

  DesktopIconSnapshot snapshot;
  if (const auto dpi = GetDpiForWindow(host)) snapshot.dpi = dpi;
  if (const auto list = FindWindowExW(host, nullptr, L"SysListView32", nullptr)) {
    DWORD_PTR font = 0;
    if (SendMessageTimeoutW(list, WM_GETFONT, 0, 0, SMTO_ABORTIFHUNG, 250, &font) && font)
      GetObjectW(reinterpret_cast<HFONT>(font), sizeof(snapshot.labelFont), &snapshot.labelFont);
  }
  if (!snapshot.labelFont.lfHeight)
    SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(snapshot.labelFont), &snapshot.labelFont, 0, snapshot.dpi);
  ComPtr<IFolderView2> folderView2;
  FOLDERVIEWMODE mode{};
  if (FAILED(folderView.As(&folderView2)) ||
      FAILED(folderView2->GetViewModeAndIconSize(&mode, &snapshot.iconSize)) || snapshot.iconSize <= 0)
    snapshot.iconSize = GetSystemMetricsForDpi(SM_CXICON, snapshot.dpi);
  POINT spacing{};
  if (SUCCEEDED(folderView->GetSpacing(&spacing)) && spacing.x > 0 && spacing.y > 0)
    snapshot.spacing = {spacing.x, spacing.y};
  else snapshot.spacing = {GetSystemMetricsForDpi(SM_CXICONSPACING, snapshot.dpi),
                           GetSystemMetricsForDpi(SM_CYICONSPACING, snapshot.dpi)};

  bool positioned = true;
  const auto complete = forEachIcon(entries.Get(), [&](PCUITEMID_CHILD entry) {
    POINT position{};
    auto path = desktopPath(desktop.Get(), entry);
    if (path.empty() || folderView->GetItemPosition(entry, &position) != S_OK || !ClientToScreen(host, &position)) {
      positioned = false;
      return;
    }
    // The renderer centers the image horizontally and starts it six DIPs below the cell top.
    position.x += snapshot.iconSize / 2;
    position.y += snapshot.spacing.cy / 2 - MulDiv(6, snapshot.dpi, 96);
    snapshot.items.push_back({std::move(path), position});
  });
  if (!complete || !positioned) return std::nullopt;
  return snapshot;
}

bool desktopSnapToGrid() {
  const auto view = desktopView();
  ComPtr<IFolderView2> folder;
  HWND host = nullptr;
  if (!view || FAILED(view->GetWindow(&host)) || !host) return false;
  const auto list = FindWindowExW(host, nullptr, L"SysListView32", nullptr);
  DWORD_PTR style = 0;
  if (list && SendMessageTimeoutW(list, LVM_GETEXTENDEDLISTVIEWSTYLE, 0, 0, SMTO_ABORTIFHUNG, 100, &style))
    return (style & LVS_EX_SNAPTOGRID) != 0;
  DWORD flags = 0;
  return SUCCEEDED(view.As(&folder)) && SUCCEEDED(folder->GetCurrentFolderFlags(&flags)) && (flags & FWF_SNAPTOGRID);
}

std::optional<std::vector<std::wstring>> desktopIconPaths() {
  ComPtr<IShellFolder> desktop;
  if (FAILED(SHGetDesktopFolder(&desktop))) return std::nullopt;
  SHELLSTATE settings{};
  SHGetSetSettings(&settings, SSF_SHOWALLOBJECTS | SSF_SHOWSUPERHIDDEN, FALSE);
  SHCONTF flags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
  if (settings.fShowAllObjects) flags |= SHCONTF_INCLUDEHIDDEN;
  if (settings.fShowSuperHidden) flags |= SHCONTF_INCLUDESUPERHIDDEN;
  ComPtr<IEnumIDList> entries;
  if (FAILED(desktop->EnumObjects(nullptr, flags, &entries))) return std::nullopt;
  std::vector<std::wstring> paths;
  const auto complete = forEachIcon(entries.Get(), [&](PCUITEMID_CHILD entry) {
    if (auto path = desktopPath(desktop.Get(), entry); !path.empty()) paths.push_back(std::move(path));
  });
  if (!complete) return std::nullopt;
  return paths;
}
}
