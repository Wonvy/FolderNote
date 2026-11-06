#include "navigation.h"
#include "explorer_tabs.h"
#include <string>
namespace navigation {
namespace {
struct Request { TabsActivation status; UINT message; };
BOOL CALLBACK syncWindow(HWND window, LPARAM context) {
  wchar_t name[64]{};
  GetClassNameW(window, name, ARRAYSIZE(name));
  if (wcscmp(name, L"CabinetWClass") && wcscmp(name, L"ExploreWClass")) return TRUE;
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return TRUE;
  wchar_t path[32768]{}, windows[MAX_PATH]{}; DWORD length = ARRAYSIZE(path);
  const BOOL queried = QueryFullProcessImageNameW(process, 0, path, &length);
  CloseHandle(process);
  if (!queried || !GetWindowsDirectoryW(windows, ARRAYSIZE(windows)) ||
      _wcsicmp(path, (std::wstring(windows) + L"\\explorer.exe").c_str())) return TRUE;
  auto &request = *reinterpret_cast<Request *>(context);
  ++request.status.windows;
  // The loaded BHO owns the band's lifecycle. ShowBrowserBar returning S_OK
  // does not establish that Explorer actually created and displayed a bar.
  DWORD_PTR acknowledged = 0;
  SetLastError(ERROR_SUCCESS);
  if (SendMessageTimeoutW(window, request.message, 0, 0, SMTO_ABORTIFHUNG,
                          1000, &acknowledged)) {
    if (acknowledged == 1) ++request.status.requested;
  } else {
    const auto error = GetLastError();
    request.status.result = HRESULT_FROM_WIN32(error ? error : ERROR_TIMEOUT);
  }
  const auto band = static_cast<HWND>(GetPropW(window, tabs::windowProperty));
  RECT bounds{};
  if (IsWindow(band) && IsWindowVisible(band) && GetWindowRect(band, &bounds) &&
      bounds.bottom > bounds.top && bounds.right > bounds.left) ++request.status.visible;
  return TRUE;
}
}
TabsActivation syncTabs() {
  Request request{{}, RegisterWindowMessageW(L"FolderNote.Navigation.SyncTabs.v1")};
  if (!tabsSupported() || !request.message) return request.status;
  EnumWindows(syncWindow, reinterpret_cast<LPARAM>(&request));
  auto &status = request.status;
  if (status.windows && status.requested == status.windows && SUCCEEDED(status.result) &&
      (tabsEnabled() ? status.visible == status.windows : status.visible == 0)) status.result = S_OK;
  constexpr auto key = L"Software\\FolderNote";
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsLastActivationResult", REG_DWORD, &status.result, sizeof(status.result));
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsActivationWindows", REG_DWORD, &status.windows, sizeof(DWORD));
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsActivationRequested", REG_DWORD, &status.requested, sizeof(DWORD));
  RegSetKeyValueW(HKEY_CURRENT_USER, key, L"TabsActivationVisible", REG_DWORD, &status.visible, sizeof(DWORD));
  return status;
}
}
