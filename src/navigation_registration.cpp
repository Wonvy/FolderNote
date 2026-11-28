#include "navigation_registration.h"
#include "language.h"
#include "explorer_tabs.h"
#include <sddl.h>
#include <shellapi.h>

namespace navreg {
static std::wstring ownerPath(const std::wstring &sid) {
  if (!validSid(sid))
    throw fn::Error{lang::text(L"安装用户标识无效，已停止注册导航扩展。")};
  return std::wstring(machineKey) + L"\\Owners\\" + sid;
}

bool validSid(const std::wstring &sid) {
  // Require the canonical SID spelling, excluding separators, quotes and args.
  if (sid.empty() || sid.size() > 184 || sid.find_first_not_of(L"S-0123456789") != std::wstring::npos)
    return false;
  PSID binary = nullptr;
  if (!ConvertStringSidToSidW(sid.c_str(), &binary))
    return false;
  LPWSTR canonical = nullptr;
  bool ok = IsValidSid(binary) && ConvertSidToStringSidW(binary, &canonical) && sid == canonical;
  if (canonical)
    LocalFree(canonical);
  LocalFree(binary);
  return ok;
}

std::wstring currentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    fn::fail(L"无法确定安装用户");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> data(size);
  BOOL read = GetTokenInformation(token, TokenUser, data.data(), size, &size);
  DWORD error = GetLastError();
  CloseHandle(token);
  if (!read)
    fn::fail(L"无法确定安装用户", error);
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid))
    fn::fail(L"无法读取安装用户标识");
  std::wstring result(sid);
  LocalFree(sid);
  return result;
}

bool machineRegistered(const std::wstring &sid) {
  HKEY key = nullptr;
  auto error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ownerPath(sid).c_str(), 0,
                             KEY_READ | KEY_WOW64_64KEY, &key);
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    return false;
  if (error)
    fn::fail(L"无法检查导航扩展注册", error);
  RegCloseKey(key);
  return true;
}

static bool legacyToolbarRegistered() {
  HKEY key = nullptr;
  auto error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, 0,
                             KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    return false;
  if (error)
    fn::fail(L"无法检查旧版标签工具栏注册", error);
  error = RegQueryValueExW(key, tabs::clsidText, nullptr, nullptr, nullptr, nullptr);
  RegCloseKey(key);
  if (error == ERROR_FILE_NOT_FOUND)
    return false;
  if (error)
    fn::fail(L"无法检查旧版标签工具栏注册", error);
  return true;
}

static void removeLegacyToolbar() {
  // The BHO hosts the tab strip directly. Remove only our old discovery value;
  // keep other tools' values and the shared Toolbar key intact.
  HKEY key = nullptr;
  auto error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, tabs::toolbarKey, 0,
                             KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    return;
  if (error)
    fn::fail(L"无法移除旧版标签工具栏注册", error);
  error = RegDeleteValueW(key, tabs::clsidText);
  RegCloseKey(key);
  if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND)
    fn::fail(L"无法移除旧版标签工具栏注册", error);
}

void registerMachine(const std::wstring &sid) {
  const auto owner = ownerPath(sid);
  HKEY key = nullptr;
  auto error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, machineKey, 0, nullptr, 0,
                               KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if (error)
    fn::fail(L"自动加载导航扩展需要管理员授权", error);
  DWORD noInternetExplorer = 1;
  error = RegSetValueExW(key, L"NoInternetExplorer", 0, REG_DWORD,
                         reinterpret_cast<const BYTE *>(&noInternetExplorer), sizeof(DWORD));
  RegCloseKey(key);
  if (error)
    fn::fail(L"无法注册导航扩展", error);
  error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, owner.c_str(), 0, nullptr, 0,
                           KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if (error)
    fn::fail(L"无法登记导航扩展用户", error);
  RegCloseKey(key);
  removeLegacyToolbar();
}

void unregisterMachine(const std::wstring &sid) {
  const auto owner = ownerPath(sid);
  // Remove just this user's claim. Another user's registration must survive.
  auto error = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, owner.c_str(), KEY_WOW64_64KEY, 0);
  if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    fn::fail(L"无法移除导航扩展用户注册", error);
  HKEY key = nullptr;
  auto owners = std::wstring(machineKey) + L"\\Owners";
  error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, owners.c_str(), 0,
                        KEY_READ | KEY_WOW64_64KEY, &key);
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
    return;
  if (error)
    fn::fail(L"无法检查其他导航扩展用户", error);
  DWORD count = 0;
  error = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &count, nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr, nullptr);
  RegCloseKey(key);
  if (error)
    fn::fail(L"无法检查其他导航扩展用户", error);
  if (count)
    return;
  // RegDeleteKeyEx refuses nonempty keys, preserving any concurrent registration.
  error = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, owners.c_str(), KEY_WOW64_64KEY, 0);
  if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND)
    fn::fail(L"无法移除导航扩展注册", error);
  error = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, machineKey, KEY_WOW64_64KEY, 0);
  if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND)
    fn::fail(L"无法移除导航扩展注册", error);
  removeLegacyToolbar();
}

static void elevate(const fn::fs::path &helper, bool install, const std::wstring &sid) {
  if (!validSid(sid))
    throw fn::Error{lang::text(L"安装用户标识无效。")};
  std::wstring arguments = install ? L"--register-navigation " : L"--unregister-navigation ";
  arguments += sid;
  SHELLEXECUTEINFOW execute{sizeof(execute)};
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute.lpVerb = L"runas";
  execute.lpFile = helper.c_str();
  execute.lpParameters = arguments.c_str();
  execute.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execute))
    fn::fail(L"导航扩展的管理员授权未完成");
  if (!execute.hProcess)
    throw fn::Error{lang::text(L"无法确认导航扩展注册结果。")};
  WaitForSingleObject(execute.hProcess, INFINITE);
  DWORD result = 1;
  GetExitCodeProcess(execute.hProcess, &result);
  CloseHandle(execute.hProcess);
  if (result)
    throw fn::Error{lang::text(L"导航扩展注册未完成，请重新运行安装或卸载程序并允许管理员授权。")};
}

static bool requiresMachineRegistration(const std::wstring &sid) {
  // Upgrades must migrate the old toolbar entry even when this user's BHO
  // ownership was already registered by an earlier version.
  return !machineRegistered(sid) || legacyToolbarRegistered();
}

bool requiresMachineRegistration() {
  return requiresMachineRegistration(currentUserSid());
}

void ensureMachine(const fn::fs::path &helper, bool allowElevation) {
  const auto sid = currentUserSid();
  if (!requiresMachineRegistration(sid)) return;
  if (!allowElevation)
    throw fn::Error{lang::text(L"导航扩展注册未完成，请重新运行安装或卸载程序并允许管理员授权。")};
  elevate(helper, true, sid);
  if (requiresMachineRegistration(sid))
    throw fn::Error{lang::text(L"导航扩展注册结果未通过检查。")};
}

void removeMachine(const fn::fs::path &helper) {
  auto sid = currentUserSid();
  if (machineRegistered(sid))
    elevate(helper, false, sid);
}

void registerUser(const fn::fs::path &directory) {
  for (auto classKey : {userKey, tabs::userKey}) {
  HKEY key = nullptr;
  auto path = std::wstring(classKey) + L"\\InprocServer32";
  auto error = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                               KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if (error)
    fn::fail(L"无法注册当前用户导航扩展", error);
  const auto dll = (directory / L"ShibaKitNavigation.dll").native();
  error = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(dll.c_str()),
                         static_cast<DWORD>((dll.size() + 1) * sizeof(wchar_t)));
  if (!error) {
    constexpr wchar_t model[] = L"Apartment";
    error = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(model), sizeof(model));
  }
  RegCloseKey(key);
  if (error)
    fn::fail(L"无法注册当前用户导航扩展", error);
  }
  const auto *title = lang::text(L"ShibaKit 文件夹标签");
  auto error = RegSetKeyValueW(HKEY_CURRENT_USER, tabs::userKey, nullptr, REG_SZ, title,
                               static_cast<DWORD>((wcslen(title) + 1) * sizeof(wchar_t)));
  if (error) fn::fail(L"无法注册标签工具栏名称", error);
}

void unregisterUser() {
  for (auto classKey : {userKey, tabs::userKey}) {
  auto error = RegDeleteTreeW(HKEY_CURRENT_USER, classKey);
  if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
    fn::fail(L"无法移除当前用户导航扩展", error);
  }
}

bool handleHelper(int count, wchar_t **arguments, int &result) {
  if (count < 2)
    return false;
  std::wstring mode = arguments[1];
  if (mode != L"--register-navigation" && mode != L"--unregister-navigation")
    return false;
  result = 1;
  if (count != 3 || !validSid(arguments[2]))
    return true;
  try {
    if (mode == L"--register-navigation")
      registerMachine(arguments[2]);
    else
      unregisterMachine(arguments[2]);
    result = 0;
  } catch (...) {
  }
  return true;
}
} // namespace navreg
