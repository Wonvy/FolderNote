#pragma once
#include "../src/quick_find_settings.h"
#include <stdexcept>
#include <string>

// Redirect HKCU within this process, including its worker threads. Never change
// the user's search preferences when testing different activation modes.
struct QuickFindTestSettings {
  HKEY user = nullptr, temporary = nullptr;
  std::wstring path;
  QuickFindTestSettings() {
    if (RegOpenCurrentUser(KEY_ALL_ACCESS, &user) != ERROR_SUCCESS) throw std::runtime_error("open test registry");
    path = L"Software\\FolderNote-QuickFindTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    if (RegCreateKeyExW(user, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &temporary, nullptr) != ERROR_SUCCESS ||
        RegOverridePredefKey(HKEY_CURRENT_USER, temporary) != ERROR_SUCCESS) {
      if (temporary) RegCloseKey(temporary);
      RegDeleteTreeW(user, path.c_str()); RegCloseKey(user);
      throw std::runtime_error("isolate search settings");
    }
  }
  ~QuickFindTestSettings() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(temporary); RegDeleteTreeW(user, path.c_str()); RegCloseKey(user);
  }
};
