#include "desktop_groups_icon_source.h"
#include "desktop_groups_model.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <memory>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto recycleBin = L"::{645FF040-5081-101B-9F08-00AA002F954E}";

std::wstring parsingPath(IShellItem *item) {
  PWSTR raw = nullptr;
  const auto result = item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &raw);
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
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    if (SUCCEEDED(items->GetItemAt(i, &item)) && item && samePath(parsingPath(item.Get()), recycleBin))
      return {recycleBin};
  }
  return {};
}
}
