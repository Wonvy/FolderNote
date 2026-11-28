#pragma once
#include "core.h"
#include "explorer_entries.h"
#include <string_view>

namespace explorer_entries {
namespace removal_detail {
inline constexpr wchar_t settingsKey[] = LR"(Software\ShibaKit)";
inline constexpr wchar_t markerPrefix[] = L"RemovedExplorerEntry.";

inline bool missing(LSTATUS status) {
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

inline std::wstring valueName(Surface surface, const std::wstring &id) {
  const std::wstring prefix = markerPrefix;
  switch (surface) {
    case Surface::ThisPc: return prefix + L"ThisPC." + id;
    case Surface::Navigation: return prefix + L"Navigation." + id;
    case Surface::Desktop: return prefix + L"Desktop." + id;
  }
  fn::fail(L"资源管理器入口位置无效", ERROR_INVALID_PARAMETER);
}
}

inline bool isRemoved(Surface surface, const std::wstring &id) {
  const auto name = removal_detail::valueName(surface, id);
  DWORD value = 0, size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, removal_detail::settingsKey, name.c_str(), RRF_RT_REG_DWORD,
                     nullptr, &value, &size) == ERROR_SUCCESS && value != 0;
}

inline void markRemoved(Surface surface, const std::wstring &id, bool removed) {
  if (id.empty()) fn::fail(L"无法修改资源管理器入口", ERROR_INVALID_PARAMETER);
  const auto name = removal_detail::valueName(surface, id);
  // Registry value names are limited to 16,383 UTF-16 characters.
  if (name.size() > 16383) fn::fail(L"无法修改资源管理器入口", ERROR_FILENAME_EXCED_RANGE);
  constexpr DWORD value = 1;
  const auto status = removed
      ? RegSetKeyValueW(HKEY_CURRENT_USER, removal_detail::settingsKey, name.c_str(), REG_DWORD, &value, sizeof(value))
      : RegDeleteKeyValueW(HKEY_CURRENT_USER, removal_detail::settingsKey, name.c_str());
  if (status && (removed || !removal_detail::missing(status)))
    fn::fail(L"无法修改资源管理器入口", status);
}

inline void clearRemoved() {
  struct Key {
    HKEY handle = nullptr;
    ~Key() { if (handle) RegCloseKey(handle); }
  } key;
  auto status = RegOpenKeyExW(HKEY_CURRENT_USER, removal_detail::settingsKey, 0,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, &key.handle);
  if (removal_detail::missing(status)) return;
  if (status) fn::fail(L"无法恢复资源管理器入口", status);

  // Gather first so deleting one marker cannot change the next index.
  std::wstring name(16384, L'\0');
  std::vector<std::wstring> markers;
  for (DWORD index = 0;; ++index) {
    DWORD length = static_cast<DWORD>(name.size());
    status = RegEnumValueW(key.handle, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status) fn::fail(L"无法读取资源管理器入口", status);
    std::wstring_view value(name.data(), length);
    if (value.starts_with(removal_detail::markerPrefix)) markers.emplace_back(value);
  }
  for (const auto &marker : markers) {
    status = RegDeleteValueW(key.handle, marker.c_str());
    if (status && !removal_detail::missing(status)) fn::fail(L"无法恢复资源管理器入口", status);
  }
}
}
