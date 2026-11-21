#include "navigation.h"
#include "navigation_registration.h"
#include "app_version.h"
#include "explorer_tabs.h"
#include "quick_find_external.h"
#include <tlhelp32.h>
#include <set>
#include <sstream>

namespace navigation {
namespace {
std::wstring stringValue(HKEY hive, const wchar_t *key, const wchar_t *name) {
  DWORD size = 0;
  if (RegGetValueW(hive, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                   nullptr, nullptr, &size) != ERROR_SUCCESS || size > 65536)
    return L"（未注册或不可读取）";
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(hive, key, name, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                   nullptr, value.data(), &size) != ERROR_SUCCESS)
    return L"（不可读取）";
  value.resize(wcsnlen(value.c_str(), value.size()));
  return value;
}
bool keyExists(HKEY hive, const wchar_t *key) {
  HKEY handle = nullptr;
  const auto error = RegOpenKeyExW(hive, key, 0, KEY_READ | KEY_WOW64_64KEY, &handle);
  if (handle) RegCloseKey(handle);
  return error == ERROR_SUCCESS;
}
DWORD tabValue(const wchar_t *name) {
  DWORD value = 0, size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", name, RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value;
}
struct Windows {
  unsigned frames = 0, tabs = 0, views = 0, controllers = 0;
  std::set<DWORD> processes;
};
BOOL CALLBACK childWindow(HWND window, LPARAM data) {
  auto &state = *reinterpret_cast<Windows *>(data);
  wchar_t cls[128]{};
  GetClassNameW(window, cls, ARRAYSIZE(cls));
  if (!wcscmp(cls, L"ShellTabWindowClass")) ++state.tabs;
  if (!wcscmp(cls, L"SHELLDLL_DefView")) ++state.views;
  if (GetPropW(window, L"FolderNote.Navigation.Controller.v3")) ++state.controllers;
  return TRUE;
}
BOOL CALLBACK explorerWindow(HWND window, LPARAM data) {
  wchar_t cls[128]{};
  GetClassNameW(window, cls, ARRAYSIZE(cls));
  if (wcscmp(cls, L"CabinetWClass") && wcscmp(cls, L"ExploreWClass")) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return TRUE;
  wchar_t path[32768]{}, windows[MAX_PATH]{};
  DWORD size = ARRAYSIZE(path);
  const bool read = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
  CloseHandle(process);
  if (!read || !GetWindowsDirectoryW(windows, ARRAYSIZE(windows)) ||
      _wcsicmp(path, (std::wstring(windows) + L"\\explorer.exe").c_str())) return TRUE;
  auto &state = *reinterpret_cast<Windows *>(data);
  ++state.frames;
  state.processes.insert(pid);
  if (GetPropW(window, L"FolderNote.Navigation.Controller.v3")) ++state.controllers;
  EnumChildWindows(window, childWindow, data);
  return TRUE;
}
} // namespace

// Read only, requested explicitly from Settings. No persistent diagnostics worker.
std::wstring diagnostics() {
  std::wostringstream text;
  const auto search = quick_find::settings();
  constexpr auto windowsKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  text << L"FolderNote " << app::version << L" 诊断\r\n"
       << L"Windows 版本：" << stringValue(HKEY_LOCAL_MACHINE, windowsKey, L"DisplayVersion")
       << L"；内部版本：" << stringValue(HKEY_LOCAL_MACHINE, windowsKey, L"CurrentBuildNumber") << L"\r\n"
       << L"文件夹备注：" << (notesEnabled() ? L"开启" : L"关闭") << L"\r\n"
       << L"中键快速定位：" << (quickEnabled() ? L"开启" : L"关闭")
       << L"；文件对话框：" << (dialogQuickEnabled() ? L"开启" : L"关闭")
       << L"；中文首字母定位：" << (quick_find::external::enabled() ? L"开启" : L"关闭")
       << L"；后台组件：" << (FindWindowExW(HWND_MESSAGE, nullptr, L"FolderNote.QuickFolders.Helper.v1", nullptr) ? L"运行中" : L"未运行") << L"\r\n"
       << L"双击导航：" << (configuredAction() == Action::Parent ? L"上一级" :
                              configuredAction() == Action::Back ? L"后退" : L"关闭") << L"\r\n"
       << L"自动加载入口：" << (keyExists(HKEY_LOCAL_MACHINE, navreg::machineKey) ? L"存在" : L"缺失/不可读") << L"\r\n";
  text << L"快捷搜索范围：盒子=" << search.boxes << L"，软件列表=" << search.lists
       << L"，桌面与资源管理器=" << search.explorer << L"，系统文件窗口=" << search.dialogs
       << L"；文件夹=" << (search.openFolders ? L"打开" : L"定位") << L"；文件=" << (search.openFiles ? L"打开" : L"定位")
       << L"；鼠标位置=" << search.atPointer << L"；原生图标=" << search.showIcons << L"；可见候选=" << search.visibleRows << L"\r\n";
  text << L"Win10 多标签：" << (tabsSupported() ? (tabsEnabled() ? L"开启" : L"关闭") : L"此系统不加载")
       << L"；始终在新标签打开：" << (preferNewTabEnabled() ? L"开启" : L"关闭")
       << L"；上次激活窗口/请求/显示：" << tabValue(L"TabsActivationWindows") << L"/" << tabValue(L"TabsActivationRequested") << L"/" << tabValue(L"TabsActivationVisible")
       << L"；结果：0x" << std::hex << tabValue(L"TabsLastActivationResult") << std::dec << L"\r\n"
       << L"标签栏加载阶段：" << stringValue(HKEY_CURRENT_USER, L"Software\\FolderNote", L"TabsLastBandStage")
       << L"；结果：0x" << std::hex << tabValue(L"TabsLastBandResult") << std::dec << L"\r\n"
       << L"标签栏模块：" << stringValue(HKEY_CURRENT_USER, L"Software\\FolderNote", L"TabsLastBandModule") << L"\r\n";
  const auto inproc = std::wstring(navreg::userKey) + L"\\InprocServer32";
  const auto registered = stringValue(HKEY_CURRENT_USER, inproc.c_str(), nullptr);
  text << L"当前注册模块：" << registered << L"\r\n";
  const DWORD attrs = GetFileAttributesW(registered.c_str());
  text << L"注册模块文件：" << (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY) ? L"存在" : L"缺失/不可读") << L"\r\n";
  Windows state;
  EnumWindows(explorerWindow, reinterpret_cast<LPARAM>(&state));
  text << L"资源管理器窗口：" << state.frames << L"；标签宿主：" << state.tabs
       << L"；文件视图：" << state.views << L"；新版导航实例：" << state.controllers << L"\r\n";
  for (DWORD pid : state.processes) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
      text << L"Explorer " << pid << L"：无法读取模块（" << GetLastError() << L"）\r\n";
      continue;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) do {
      if (_wcsicmp(entry.szModule, L"FolderNoteNavigation.dll")) continue;
      found = true;
      text << L"Explorer " << pid << L" 已加载：" << entry.szExePath
           << (_wcsicmp(entry.szExePath, registered.c_str()) ? L"（与当前注册路径不同）" : L"（当前版本路径）") << L"\r\n";
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (!found) text << L"Explorer " << pid << L"：未加载导航 DLL\r\n";
  }
  text << L"以上只读取加载状态；DLL 已加载不代表手势已完成实测。";
  return text.str();
}
} // namespace navigation
