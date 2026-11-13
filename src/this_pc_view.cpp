#include "this_pc_view.h"
#include <algorithm>
#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <memory>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string_view>
#include <wrl/client.h>

namespace this_pc_view {
namespace {
using Microsoft::WRL::ComPtr;
struct CoTaskFree {
  using pointer = PIDLIST_ABSOLUTE;
  void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, CoTaskFree>;
using ShellText = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>;

std::wstring readName(IShellFolder &folder, PCUITEMID_CHILD child, SHGDNF flags) {
  STRRET text{};
  if (FAILED(folder.GetDisplayNameOf(child, flags, &text))) return {};
  PWSTR raw = nullptr;
  if (FAILED(StrRetToStrW(&text, child, &raw))) return {};
  ShellText name(raw, CoTaskMemFree);
  return name ? name.get() : L"";
}

std::wstring shellPath(PCIDLIST_ABSOLUTE id) {
  PWSTR raw = nullptr;
  if (FAILED(SHGetNameFromIDList(id, SIGDN_FILESYSPATH, &raw))) return {};
  ShellText path(raw, CoTaskMemFree);
  return path ? path.get() : L"";
}

std::wstring classId(std::wstring_view name) {
  if (!name.starts_with(L"::{") || name.size() != 40 || name.back() != L'}') return {};
  std::wstring value(name.substr(2));
  GUID guid{};
  wchar_t canonical[39]{};
  if (FAILED(CLSIDFromString(value.c_str(), &guid)) || !StringFromGUID2(guid, canonical, ARRAYSIZE(canonical)))
    return {};
  return canonical;
}

bool driveRoot(std::wstring_view target) {
  return target.size() == 3 && target[1] == L':' && target[2] == L'\\';
}

ComPtr<ICategorizer> defaultCategory(IShellFolder &folder) {
  ComPtr<ICategoryProvider> provider;
  if (FAILED(folder.CreateViewObject(nullptr, IID_PPV_ARGS(&provider))) || !provider) return {};
  GUID category{};
  SHCOLUMNID column{};
  ComPtr<ICategorizer> result;
  if (provider->GetDefaultCategory(&category, &column) == S_OK)
    provider->CreateCategory(&category, IID_PPV_ARGS(&result));
  return result;
}

Group categoryOf(ICategorizer *category, PCUITEMID_CHILD child, const std::wstring &target,
                 std::wstring_view name) {
  if (category) {
    DWORD id = 0;
    if (SUCCEEDED(category->GetCategory(1, &child, &id))) {
      // The This PC category provider uses these resource IDs for its default groups.
      switch (id) {
      case 0x247a: return Group::Folders;
      case 0x247b: return Group::Drives;
      case 0x247c: return Group::Network;
      }
    }
  }
  if (driveRoot(target)) return GetDriveTypeW(target.c_str()) == DRIVE_REMOTE ? Group::Network : Group::Drives;
  if (target.starts_with(L"\\\\") || name.starts_with(L"uuid:")) return Group::Network;
  return target.empty() ? Group::Drives : Group::Folders;
}

struct Computer {
  Pidl id;
  ComPtr<IShellFolder> folder;
  Computer() {
    PIDLIST_ABSOLUTE raw = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, KF_FLAG_DONT_VERIFY, nullptr, &raw))) return;
    id.reset(raw);
    ComPtr<IShellFolder> desktop;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) && desktop)
      desktop->BindToObject(id.get(), nullptr, IID_PPV_ARGS(&folder));
  }
};

ComPtr<IEnumIDList> viewItems(PCIDLIST_ABSOLUTE computer) {
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows))) ||
      !windows) return {};
  LONG count = 0;
  if (FAILED(windows->get_Count(&count))) return {};
  const auto start = GetTickCount64();
  for (LONG index = 0; index < count && GetTickCount64() - start < 1000; ++index) {
    VARIANT item{};
    item.vt = VT_I4;
    item.lVal = index;
    ComPtr<IDispatch> dispatch;
    ComPtr<IServiceProvider> services;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView> folderView;
    ComPtr<IPersistFolder2> location;
    if (FAILED(windows->Item(item, &dispatch)) || !dispatch || FAILED(dispatch.As(&services)) ||
        FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser ||
        FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view.As(&folderView)) ||
        FAILED(folderView->GetFolder(IID_PPV_ARGS(&location))) || !location) continue;
    PIDLIST_ABSOLUTE raw = nullptr;
    const auto located = location->GetCurFolder(&raw);
    Pidl current(raw);
    if (located != S_OK || !current || !ILIsEqual(computer, current.get())) continue;
    ComPtr<IEnumIDList> displayed;
    if (SUCCEEDED(folderView->Items(SVGIO_ALLVIEW | SVGIO_FLAG_VIEWORDER, IID_PPV_ARGS(&displayed))) && displayed)
      return displayed;
  }
  return {};
}

struct OrderedItem {
  Pidl pidl;
  Item item;
};

int shellIcon(PCIDLIST_ABSOLUTE id) {
  SHFILEINFOW icon{};
  return id && SHGetFileInfoW(reinterpret_cast<LPCWSTR>(id), 0, &icon, sizeof(icon),
                            SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_SMALLICON) ? icon.iIcon : -1;
}

void addMissingDrives(const Computer &computer, std::vector<OrderedItem> &entries) {
  const auto drives = GetLogicalDrives() & ~static_cast<DWORD>(SHRestricted(REST_NODRIVES));
  for (int i = 0; i < 26; ++i) {
    if (!(drives & (1u << i))) continue;
    wchar_t root[]{static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
    const auto type = GetDriveTypeW(root);
    if (type != DRIVE_REMOVABLE && type != DRIVE_CDROM) continue;
    if (std::any_of(entries.begin(), entries.end(), [&](const auto &entry) {
          return driveRoot(entry.item.target) && _wcsicmp(entry.item.target.c_str(), root) == 0;
        })) continue;
    PIDLIST_RELATIVE raw = nullptr;
    const HRESULT parsed = computer.folder->ParseDisplayName(nullptr, nullptr, root, nullptr, &raw, nullptr);
    Pidl child(raw);
    if (FAILED(parsed)) child.reset();
    Pidl absolute(child ? ILCombine(computer.id.get(), child.get()) : nullptr);
    auto name = child ? readName(*computer.folder.Get(), child.get(), SHGDN_NORMAL) : std::wstring{};
    if (name.empty()) name.assign(root, 2);
    int icon = shellIcon(absolute.get());
    if (icon < 0) {
      SHSTOCKICONINFO stock{sizeof(stock)};
      if (SUCCEEDED(SHGetStockIconInfo(type == DRIVE_CDROM ? SIID_DRIVECD : SIID_DRIVEREMOVE,
                                       SHGSI_SYSICONINDEX, &stock)))
        icon = stock.iSysImageIndex;
    }
    OrderedItem entry{std::move(child), {std::wstring(root, 2), std::move(name), root, Group::Drives, icon}};
    auto after = std::find_if(entries.begin(), entries.end(), [&](const auto &other) {
      return other.item.group == Group::Drives && driveRoot(other.item.target) && other.item.target[0] > root[0];
    });
    if (after == entries.end()) {
      auto last = std::find_if(entries.rbegin(), entries.rend(), [](const auto &other) {
        return other.item.group == Group::Drives;
      });
      if (last != entries.rend()) after = last.base();
    }
    entries.insert(after, std::move(entry));
  }
}
}

std::vector<Item> items() {
  Computer computer;
  if (!computer.folder) return {};
  const auto &folder = computer.folder;
  auto children = viewItems(computer.id.get());
  const bool hasView = children != nullptr;
  if (!children) folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &children);
  if (!children) return {};
  auto category = defaultCategory(*folder.Get());
  std::vector<OrderedItem> entries;
  PITEMID_CHILD raw = nullptr;
  while (children->Next(1, &raw, nullptr) == S_OK && raw) {
    Pidl child(raw);
    const auto parsingName = readName(*folder.Get(), child.get(), SHGDN_FORPARSING | SHGDN_INFOLDER);
    auto name = readName(*folder.Get(), child.get(), SHGDN_NORMAL);
    if (name.empty()) continue;
    Pidl absolute(ILCombine(computer.id.get(), child.get()));
    auto target = absolute ? shellPath(absolute.get()) : std::wstring{};
    auto id = classId(parsingName);
    if (id.empty()) id = parsingName.empty() ? name : parsingName;
    const int iconIndex = shellIcon(absolute.get());
    const auto group = categoryOf(category.Get(), child.get(), target, parsingName);
    entries.push_back({std::move(child), {std::move(id), std::move(name), std::move(target), group, iconIndex}});
  }
  if (!hasView) std::stable_sort(entries.begin(), entries.end(), [&](const auto &a, const auto &b) {
    const HRESULT order = folder->CompareIDs(0, a.pidl.get(), b.pidl.get());
    if (SUCCEEDED(order)) return static_cast<SHORT>(HRESULT_CODE(order)) < 0;
    return StrCmpLogicalW(a.item.name.c_str(), b.item.name.c_str()) < 0;
  });
  // The detached namespace can omit removable slots that Explorer displays.
  // Keep a live view's actual visible set when it is available.
  if (!hasView) addMissingDrives(computer, entries);
  std::vector<Item> result;
  result.reserve(entries.size());
  for (auto &entry : entries) result.push_back(std::move(entry.item));
  return result;
}

Group classify(const std::wstring &id) {
  if (classId(L"::" + id).empty()) return Group::Drives;
  Computer computer;
  if (!computer.folder) return Group::Drives;
  auto name = L"::" + id;
  PIDLIST_RELATIVE raw = nullptr;
  if (FAILED(computer.folder->ParseDisplayName(nullptr, nullptr, name.data(), nullptr, &raw, nullptr)) ||
      !raw) return Group::Drives;
  Pidl child(raw);
  auto category = defaultCategory(*computer.folder.Get());
  Pidl absolute(ILCombine(computer.id.get(), child.get()));
  const auto target = absolute ? shellPath(absolute.get()) : std::wstring{};
  return categoryOf(category.Get(), child.get(), target, name);
}
}
