#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <optional>
#include <string>

namespace ui {

inline std::optional<std::wstring> pickSystemIcon(HWND owner, const std::wstring &current) {
  wchar_t system[MAX_PATH]{};
  const auto systemLength = GetSystemDirectoryW(system, ARRAYSIZE(system));
  if (!systemLength || systemLength >= ARRAYSIZE(system)) return std::nullopt;

  const std::wstring fallback = std::wstring(system) + L"\\shell32.dll";
  std::wstring initial = fallback;
  int index = 0;
  if (!current.empty()) {
    wchar_t expanded[MAX_PATH]{};
    const auto length = ExpandEnvironmentStringsW(current.c_str(), expanded, ARRAYSIZE(expanded));
    if (length && length <= ARRAYSIZE(expanded)) {
      auto iconLocation = expanded + (expanded[0] == L'@' ? 1 : 0);
      const int resource = PathParseIconLocationW(iconLocation);
      PathUnquoteSpacesW(iconLocation);
      std::wstring path = iconLocation;
      if (!path.empty() && path.find_first_of(L"\\/:") == std::wstring::npos)
        path = std::wstring(system) + L"\\" + path;
      if (!path.empty() && !PathIsRelativeW(path.c_str()) && path.size() < MAX_PATH) {
        initial = path;
        index = resource;
      }
    }
  }

  wchar_t selected[MAX_PATH]{};
  if (initial.size() >= ARRAYSIZE(selected)) return std::nullopt;
  wcscpy_s(selected, initial.c_str());
  if (PickIconDlg(owner, selected, ARRAYSIZE(selected), &index) != 1) return std::nullopt;
  selected[ARRAYSIZE(selected) - 1] = L'\0';
  wchar_t expanded[MAX_PATH]{};
  const auto length = ExpandEnvironmentStringsW(selected, expanded, ARRAYSIZE(expanded));
  if (!length || length > ARRAYSIZE(expanded) || PathIsRelativeW(expanded)) return std::nullopt;
  const auto attributes = GetFileAttributesW(expanded);
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
  return std::wstring(selected) + L"," + std::to_wstring(index);
}

} // namespace ui
