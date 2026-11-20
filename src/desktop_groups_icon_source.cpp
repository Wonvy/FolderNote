#include "desktop_groups_icon_source.h"
#include "desktop_groups_model.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <memory>
#include <utility>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto recycleBin = L"::{645FF040-5081-101B-9F08-00AA002F954E}";

std::wstring displayPath(IShellItem *item, SIGDN form) {
  PWSTR raw = nullptr;
  const auto result = item->GetDisplayName(form, &raw);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
  if (FAILED(result) || !path) return {};
  return path.get();
}

}

std::vector<std::wstring> desktopShellDropItems(IDataObject *data) {
  ComPtr<IShellItemArray> items;
  DWORD count = 0;
  if (!data || FAILED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) ||
      !items || FAILED(items->GetCount(&count))) return {};
  std::vector<std::wstring> paths;
  paths.reserve(count);
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
    if (auto path = displayPath(item.Get(), SIGDN_FILESYSPATH); !path.empty()) {
      paths.push_back(std::move(path));
    } else if (samePath(displayPath(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING), recycleBin)) {
      paths.emplace_back(recycleBin);
    }
  }
  return paths;
}
}
