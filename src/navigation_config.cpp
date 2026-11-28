#include "navigation.h"
#include <cstdint>
#include <cstdlib>

namespace navigation {
bool tabsSupported() {
  // RtlGetVersion is not affected by an application's compatibility manifest.
  using Version = LONG (WINAPI *)(OSVERSIONINFOW *);
  auto version = reinterpret_cast<Version>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  OSVERSIONINFOW info{sizeof(info)};
  return version && version(&info) == 0 && info.dwMajorVersion == 10 && info.dwBuildNumber < 22000;
}
bool tabsEnabled() {
  DWORD value = 0, size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerTabsEnabled", RRF_RT_REG_DWORD,
      nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
}
bool preferNewTabEnabled() {
  DWORD value = 0, size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"ExplorerOpenInNewTab", RRF_RT_REG_DWORD,
      nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
}
bool isDoubleClick(const Click &a, const Click &b, UINT interval, int width, int height) {
  return width >= 0 && height >= 0 && a.blank && b.blank && a.root && a.root == b.root &&
         a.view && a.view == b.view && DWORD(b.time - a.time) <= interval &&
         std::abs(std::int64_t(b.point.x) - a.point.x) <= width / 2 &&
         std::abs(std::int64_t(b.point.y) - a.point.y) <= height / 2;
}

Action configuredAction() {
  DWORD value = 0, size = sizeof(value);
  const auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"BlankDoubleClick",
                                  RRF_RT_REG_DWORD, nullptr, &value, &size);
  // First use enables Parent; an explicit Off survives upgrades and reinstalls.
  if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND)
    return Action::Parent;
  return result == ERROR_SUCCESS && value <= DWORD(Action::Back) ? Action(value) : Action::Off;
}

bool noteCommandHidden(int operation) {
  if (operation < 0 || operation >= 3) return false;
  DWORD value = 0, bytes = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"HiddenNoteCommands", RRF_RT_REG_DWORD, nullptr, &value, &bytes);
  return (value & (1u << operation)) != 0;
}
bool notesEnabled() {
  DWORD value = 0, size = sizeof(value);
  const auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"NotesEnabled",
                                  RRF_RT_REG_DWORD, nullptr, &value, &size);
  // Notes are opt-in. A saved choice is preserved by the installer.
  return result == ERROR_SUCCESS && value == 1;
}

Action preferredAction() {
  DWORD value = 0, size = sizeof(value);
  const auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"NavigationPreference",
                                  RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (result == ERROR_SUCCESS && (value == DWORD(Action::Parent) || value == DWORD(Action::Back)))
    return Action(value);
  const auto current = configuredAction();
  return current == Action::Off ? Action::Parent : current;
}
bool quickEnabled() {
  DWORD value = 0, bytes = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersEnabled",
      RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS && value == 1;
}
bool dialogQuickEnabled() {
  DWORD value = 1, bytes = sizeof(value);
  const auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersInDialogs",
      RRF_RT_REG_DWORD, nullptr, &value, &bytes);
  return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND || (result == ERROR_SUCCESS && value == 1);
}
bool elevatedDialogQuickEnabled() {
  DWORD value = 0, bytes = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersElevatedDialogs",
      RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS && value == 1;
}
void setElevatedDialogQuickEnabled(bool enabled) {
  const DWORD value = enabled ? 1UL : 0UL;
  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersElevatedDialogs",
      REG_DWORD, &value, sizeof(value));
}
} // namespace navigation
