#include "desktop_groups_menu.h"
#include "language.h"
#include <windows.h>
#include <shlobj.h>

namespace desktop_groups {
namespace {
constexpr wchar_t verbKey[] = L"Software\\Classes\\DesktopBackground\\shell\\FolderNote.DesktopGroups.Create";

LSTATUS setString(HKEY key, const wchar_t *subkey, const wchar_t *name, const std::wstring &value) {
  return RegSetKeyValueW(key, subkey, name, REG_SZ, value.c_str(),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void refresh() {
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
}
}

bool configureDesktopMenu(const std::wstring &exe) {
  if (exe.empty()) {
    const auto status = RegDeleteTreeW(HKEY_CURRENT_USER, verbKey);
    if (status == ERROR_SUCCESS) refresh();
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
  }
  const std::wstring quoted = L"\"" + exe + L"\"";
  HKEY key = nullptr;
  DWORD disposition = 0;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, verbKey, 0, nullptr, 0, KEY_CREATE_SUB_KEY | KEY_SET_VALUE,
                      nullptr, &key, &disposition) != ERROR_SUCCESS)
    return false;
  const bool updated = setString(key, L"command", nullptr, quoted + L" --desktop-groups-create") == ERROR_SUCCESS &&
      setString(key, nullptr, L"Icon", quoted + L",0") == ERROR_SUCCESS &&
      setString(key, nullptr, L"MUIVerb", lang::text(L"新建盒子")) == ERROR_SUCCESS;
  RegCloseKey(key);
  if (updated) refresh();
  else if (disposition == REG_CREATED_NEW_KEY) RegDeleteTreeW(HKEY_CURRENT_USER, verbKey);
  return updated;
}
}
