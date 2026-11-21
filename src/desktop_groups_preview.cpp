#include "desktop_groups_preview.h"
#include <windows.h>

namespace desktop_groups {
bool desktopPreviewAvailable() { return FindWindowW(L"SeerWindowClass", nullptr) != nullptr; }

HWND desktopPreviewWindow() {
  const auto endpoint = FindWindowW(L"SeerWindowClass", nullptr);
  if (!endpoint) return nullptr;
  struct Search { DWORD pid = 0; HWND window = nullptr; } search;
  GetWindowThreadProcessId(endpoint, &search.pid);
  if (!search.pid) return nullptr;
  EnumWindows([](HWND window, LPARAM context) -> BOOL {
    auto &search = *reinterpret_cast<Search *>(context);
    // A preview opened from Explorer can deliberately avoid activation.
    // That does not make it hidden, or safe for desktop boxes to cover.
    if (!IsWindowVisible(window)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search.pid) return TRUE;
    search.window = window;
    return FALSE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.window;
}

bool previewDesktopItem(const std::wstring &path) {
  if (path.empty() || path.size() > 32767 || path.find(L'\0') != std::wstring::npos ||
      GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  const auto seer = FindWindowW(L"SeerWindowClass", nullptr);
  if (!seer) return false;
  // Official lightweight IPC protocol, also supported by Seer 4.8:
  // https://github.com/ccseer/Seer-sdk/blob/main/seer/ipclegacyhelper.h
  // Seer requires at least 200ms between invoke commands.
  static ULONGLONG lastInvoke = 0;
  const auto now = GetTickCount64();
  if (lastInvoke && now - lastInvoke < 200) return true;
  DWORD pid = 0;
  GetWindowThreadProcessId(seer, &pid);
  if (pid) AllowSetForegroundWindow(pid);
  COPYDATASTRUCT data{5000, static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)),
                      const_cast<wchar_t *>(path.c_str())};
  DWORD_PTR reply = 0;
  // Seer 4.8 returns zero even after a successful preview. Delivery, rather
  // than its window procedure's return value, determines whether we handled it.
  const bool delivered = SendMessageTimeoutW(seer, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
      SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 300, &reply) != 0;
  if (delivered) {
    lastInvoke = now;
    // Give the requested preview keyboard focus and leave Show Desktop mode.
    // Do not show it ourselves: a second invoke intentionally hides it.
    if (const auto viewer = desktopPreviewWindow()) SetForegroundWindow(viewer);
  }
  return delivered;
}
}
