#include "quick_find_settings.h"

namespace quick_find {
namespace {
constexpr auto key = L"Software\\ShibaKit";
constexpr auto name = L"QuickFindOptions";
constexpr DWORD version = 0x10000000;
}
Settings settings() {
  Settings result;
  DWORD value = 0, size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
      (value & 0xffff0000) == version && (value >> 8 & 0xff) >= 5 && (value >> 8 & 0xff) <= 12) {
    result.boxes = (value & 1) != 0; result.lists = (value & 2) != 0;
    result.explorer = (value & 4) != 0; result.dialogs = (value & 8) != 0;
    result.openFolders = (value & 16) != 0; result.openFiles = (value & 32) != 0;
    result.atPointer = (value & 64) != 0; result.showIcons = (value & 128) != 0;
    result.visibleRows = value >> 8 & 0xff;
  } else {
    value = 0; size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"PinyinQuickFind", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
      result.explorer = result.dialogs = value == 1;
  }
  return result;
}
LSTATUS saveSettings(const Settings &s) {
  if (s.visibleRows < 5 || s.visibleRows > 12) return ERROR_INVALID_PARAMETER;
  const DWORD value = version | (s.boxes ? 1 : 0) | (s.lists ? 2 : 0) | (s.explorer ? 4 : 0) |
      (s.dialogs ? 8 : 0) | (s.openFolders ? 16 : 0) | (s.openFiles ? 32 : 0) |
      (s.atPointer ? 64 : 0) | (s.showIcons ? 128 : 0) | (s.visibleRows << 8);
  return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_DWORD, &value, sizeof(value));
}
}
