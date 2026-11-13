#include "core.h"
#include "language.h"
#include "context_commands.h"
#include "context_inventory.h"
#include "custom_explorer_entries.h"
#include "desktop_folder_links.h"
#include "explorer_entries.h"
#include "navigation.h"
#include "navigation_registration.h"
#include "resource.h"
#include "removed_entries.h"
#include "quick_folders.h"
#include "window_position.h"
#include "app_version.h"
#include <shellapi.h>
#include <shlobj.h>
static bool quiet = false;
static int show(HWND h, LPCWSTR text, LPCWSTR title, UINT flags) {
  if (quiet)
    return (flags & MB_YESNO) ? IDYES : IDOK;
  return ui::message(h, text, title, flags);
}
#define MessageBoxW show
static void value(const std::wstring &path, const wchar_t *name, const std::wstring &data) {
  HKEY key{};
  auto e = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                           nullptr);
  if (e)
    fn::fail(L"无法注册菜单", e);
  e = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)data.c_str(),
                     (DWORD)((data.size() + 1) * 2));
  RegCloseKey(key);
  if (e)
    fn::fail(L"无法写入菜单", e);
}
static void removeKey(const std::wstring &p) {
  auto e = RegDeleteTreeW(HKEY_CURRENT_USER, p.c_str());
  if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND)
    fn::fail(L"无法移除菜单", e);
}
static fn::fs::path settingsShortcut() {
#ifdef FOLDERNOTE_UNINSTALL_TEST
  return fn::localRoot().parent_path() / L"StartMenu" / L"FolderNote 设置.lnk";
#else
  PWSTR programs = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs)))
    throw fn::Error{lang::text(L"无法定位当前用户的开始菜单。")};
  fn::fs::path path = fn::fs::path(programs) / L"FolderNote" / L"FolderNote 设置.lnk";
  CoTaskMemFree(programs);
  return path;
#endif
}
static void createSettingsShortcut(const fn::fs::path &active) {
  auto path = settingsShortcut();
  fn::fs::create_directories(path.parent_path());
  IShellLinkW *link = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
  if (SUCCEEDED(hr)) {
    hr = link->SetPath((active / L"FolderNote.exe").c_str());
    if (SUCCEEDED(hr)) hr = link->SetArguments(L"--settings");
    if (SUCCEEDED(hr)) hr = link->SetDescription(lang::text(L"设置文件夹备注、双击导航、中键快速定位和外观主题"));
    if (SUCCEEDED(hr)) hr = link->SetIconLocation((active / L"FolderNote.exe").c_str(), -IDI_FOLDERNOTE);
    IPersistFile *file = nullptr;
    if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) hr = file->Save(path.c_str(), TRUE);
    if (file) file->Release();
    link->Release();
  }
  if (FAILED(hr)) throw fn::Error{lang::text(L"无法创建 FolderNote 设置的开始菜单入口。")};
}
static void registerMenus(const fn::fs::path &active, bool modern) {
  const auto icon = L"\"" + (active / L"FolderNote.exe").native() + L"\",-" +
                    std::to_wstring(IDI_FOLDERNOTE);
  for (int i = 0; i < 3; ++i) {
    const wchar_t *id = fn::ids[i];
    auto cls = std::wstring(L"Software\\Classes\\CLSID\\") + id + L"\\InprocServer32";
    value(cls, nullptr, (active / L"FolderNoteShell.dll").native());
    value(cls, L"ThreadingModel", L"Apartment");
  }
  for (int i = 0; i < 3; ++i) {
    auto verb = std::wstring(L"Software\\Classes\\Directory\\shell\\") + fn::verbs[i];
    if (modern) {
      removeKey(verb); // The package owns the first-level commands; avoid duplicates.
      continue;
    }
    value(verb, nullptr, lang::text(fn::titles[i]));
    value(verb, L"Icon", icon);
    value(verb, L"MultiSelectModel", i == 0 ? L"Single" : L"Player");
    value(verb + L"\\command", L"DelegateExecute", fn::ids[i]);
  }
  // Settings live in Start, including when both features are off. Remove the
  // old always-visible context entries when upgrading from 1.4.0.
  removeKey(L"Software\\Classes\\Directory\\shell\\FolderNote.Settings");
  removeKey(L"Software\\Classes\\Directory\\Background\\shell\\FolderNote.Settings");
  removeKey(std::wstring(L"Software\\Classes\\CLSID\\") + fn::settingsId);
  // Persist first-install defaults so previously cached modules also see them.
  // Existing choices (including Off) are read before writing and preserved.
  navigation::saveSettings(navigation::notesEnabled(), navigation::configuredAction(),
                           navigation::preferredAction());
}
static constexpr const wchar_t* payloadNames[]={L"FolderNote.exe",L"FolderNoteShell.dll",L"FolderNoteNavigation.dll",L"Install.exe",L"Uninstall.exe"};
static bool isVersionDirectory(const fn::fs::path& p) {
  const auto name=p.filename().native();GUID id{};
  return name.size()==38 && name.front()==L'{' && name.back()==L'}' && SUCCEEDED(CLSIDFromString(name.c_str(),&id));
}
static void ordinaryDirectory(const fn::fs::path& p) {
  DWORD a=GetFileAttributesW(p.c_str());
  if(a==INVALID_FILE_ATTRIBUTES || !(a&FILE_ATTRIBUTE_DIRECTORY) || (a&FILE_ATTRIBUTE_REPARSE_POINT))
    throw fn::Error{lang::format(L"安装目录不是普通文件夹，已停止操作：{0}", {p.native()})};
}
static void removePayload(const fn::fs::path& p,std::wstring& remaining) {
  DWORD attrs=GetFileAttributesW(p.c_str());
  if(attrs==INVALID_FILE_ATTRIBUTES)return;
  if(attrs&FILE_ATTRIBUTE_REPARSE_POINT){remaining+=lang::format(L"{0}（目录链接，未处理）\n", {p.native()});return;}
  for(auto name:payloadNames)if(!DeleteFileW((p/name).c_str())) {
    DWORD code=GetLastError();if(code!=ERROR_FILE_NOT_FOUND&&code!=ERROR_PATH_NOT_FOUND)remaining+=(p/name).native()+L"\n";
  }
  RemoveDirectoryW(p.c_str()); // Only remove an empty folder; preserve unrelated files.
}
static fn::fs::path stageVersion(const fn::fs::path& source,const fn::fs::path& root) {
  for(auto name:payloadNames)if(GetFileAttributesW((source/name).c_str())==INVALID_FILE_ATTRIBUTES)
    throw fn::Error{lang::text(L"安装包不完整，请完整解压后运行 Install.exe。")};
  fn::fs::create_directories(root);ordinaryDirectory(root);
  auto versions=root/L"Versions";fn::fs::create_directory(versions);ordinaryDirectory(versions);
  GUID id; if(FAILED(CoCreateGuid(&id)))throw fn::Error{lang::text(L"无法生成安装版本标识。")};
  wchar_t token[40];StringFromGUID2(id,token,40);auto version=versions/token;
  if(!fn::fs::create_directory(version))throw fn::Error{lang::text(L"版本目录已经存在，安装未继续。")};
  try {for(auto name:payloadNames)if(!CopyFileW((source/name).c_str(),(version/name).c_str(),TRUE))fn::fail(L"无法复制新版程序文件");}
  catch(...){std::wstring unused;removePayload(version,unused);throw;}
  return version;
}
#ifdef FOLDERNOTE_UNINSTALL_ONLY
// Remove only our exact package identity for the current user. Never enumerate
// or remove other users' packages, and never change system trust policies.
static bool removeModernPackage() {
#ifdef FOLDERNOTE_UNINSTALL_TEST
  return true;
#else
  wchar_t system[MAX_PATH];
  GetSystemDirectoryW(system, MAX_PATH);
  auto ps = fn::fs::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
  std::wstring command = L"\"" + ps.native() +
                         L"\" -NoProfile -NonInteractive -Command \"try { Get-AppxPackage -Name "
                         L"'FolderNote.Desktop' -ErrorAction Stop | Where-Object { $_.Publisher "
                         L"-eq 'CN=FolderNote, OID.2.25.311729368913984317654407730594956997722=1' "
                         L"} | ForEach-Object { Remove-AppxPackage -Package $_.PackageFullName "
                         L"-ErrorAction Stop }; exit 0 } catch { exit 1 }\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(ps.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &si, &pi))
    return false;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
#endif
}
#endif
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int n;
  auto args = CommandLineToArgvW(GetCommandLineW(), &n);
  int helperResult = 0;
  if (navreg::handleHelper(n, args, helperResult)) {
    LocalFree(args);
    CoUninitialize();
    return helperResult;
  }
  for (int i = 1; i < n; ++i)
    if (std::wstring(args[i]) == L"--quiet")
      quiet = true;
  int result = 0;
  try {
    auto dest = fn::localRoot();
    auto self = fn::modulePath();
    bool uninstall = n > 1 && std::wstring(args[1]) == L"--uninstall";
    bool worker = n > 1 && std::wstring(args[1]) == L"--uninstall-worker";
    bool navigationOnly = n > 1 && std::wstring(args[1]) == L"--navigation-only";
#ifdef FOLDERNOTE_UNINSTALL_ONLY
    uninstall = !worker;
#endif
    for (int i = 1; i + 1 < n; ++i)
      if (std::wstring(args[i]) == L"--parent") {
        DWORD pid = wcstoul(args[i + 1], nullptr, 10);
        if (pid && pid != GetCurrentProcessId()) {
          HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
          if (parent) {
            WaitForSingleObject(parent, 10000);
            CloseHandle(parent);
          }
        }
      }
    if (uninstall &&
        MessageBoxW(nullptr,
#ifdef FOLDERNOTE_UNINSTALL_ONLY
                    lang::text(L"卸载当前用户的 FolderNote（通用版及 Win11 "
                    L"首层菜单版），移除双击导航扩展及右键菜单？\n\n移除自动加载注册可能需要管理员授权。所有文件夹中的备注都会保留。"),
#else
                    lang::text(L"移除 FolderNote 和右键菜单？已保存的文件夹备注会保留。"),
#endif
                    L"FolderNote", MB_YESNO | MB_ICONQUESTION) != IDYES) {
      LocalFree(args);
      CoUninitialize();
      return 0;
    }
    if (uninstall && (self.parent_path() == dest ||
        (self.parent_path().parent_path()==dest/L"Versions" && isVersionDirectory(self.parent_path())))) {
      auto temp = fn::fs::temp_directory_path() /
                  (L"FolderNote-Uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
      if (!CopyFileW(self.c_str(), temp.c_str(), TRUE))
        fn::fail(L"无法启动卸载");
      std::wstring cmd = L"\"" + temp.native() + L"\" --uninstall-worker --parent " +
                         std::to_wstring(GetCurrentProcessId());
      if (quiet)
        cmd += L" --quiet";
      STARTUPINFOW si{sizeof(si)};
      PROCESS_INFORMATION pi{};
      if (!CreateProcessW(temp.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                          &si, &pi))
        fn::fail(L"无法启动卸载");
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    } else if (uninstall || worker) {
      // Complete privileged registration removal before touching user files.
      // The elevated helper never accesses the original user's HKCU or files.
#ifdef FOLDERNOTE_UNINSTALL_TEST
      navreg::unregisterMachine(navreg::currentUserSid());
#else
      navreg::removeMachine(self);
#endif
      navigation::stop();
#ifndef FOLDERNOTE_UNINSTALL_TEST
      quick::stopHelper();
#endif
      navreg::unregisterUser();
      context::unregisterCommands();
      const auto unresolvedContextMenus = context::restoreAll();
      size_t unresolvedEntries = 0;
      for (auto cleanup : {explorer_entries::restoreAll, desktop_folder_links::restoreAll}) {
        try { unresolvedEntries += cleanup(); }
        catch (...) { ++unresolvedEntries; }
      }
      if (!unresolvedEntries) {
        try { explorer_entries::clearRemoved(); }
        catch (...) { ++unresolvedEntries; }
      }
      try { unresolvedEntries += explorer_entries::removeAllCustom(); }
      catch (...) { ++unresolvedEntries; }
#ifdef FOLDERNOTE_UNINSTALL_ONLY
      bool modernRemoved = removeModernPackage();
#endif
      removeKey(L"Software\\Classes\\Directory\\Background\\shell\\FolderNote.Settings");
      removeKey(L"Software\\Classes\\Directory\\shell\\FolderNote.Settings");
      removeKey(std::wstring(L"Software\\Classes\\CLSID\\") + fn::settingsId);
      for (int i = 0; i < 3; ++i) {
        removeKey(std::wstring(L"Software\\Classes\\Directory\\shell\\") + fn::verbs[i]);
        removeKey(std::wstring(L"Software\\Classes\\CLSID\\") + fn::ids[i]);
      }
      removeKey(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Fold"
                L"erNote");
      auto shortcut = settingsShortcut();
      if (!DeleteFileW(shortcut.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND &&
          GetLastError() != ERROR_PATH_NOT_FOUND)
        fn::fail(L"无法移除设置快捷方式");
      RemoveDirectoryW(shortcut.parent_path().c_str());
      SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
      std::wstring remaining;
      auto versions=dest/L"Versions";
      if(fn::fs::exists(dest))ordinaryDirectory(dest);
      if(fn::fs::exists(versions)) {
        ordinaryDirectory(versions);
        for(const auto& entry:fn::fs::directory_iterator(versions))
          if(isVersionDirectory(entry.path()))removePayload(entry.path(),remaining);
        RemoveDirectoryW(versions.c_str());
      }
      removePayload(dest,remaining);
      RemoveDirectoryW((dest / L"Requests").c_str());
      RemoveDirectoryW(dest.c_str());
#ifdef FOLDERNOTE_UNINSTALL_ONLY
      if (!modernRemoved)
        throw fn::Error{lang::text(L"通用版卸载已执行，但 Win11 应用包未能移除。请在 Windows 设置 → "
                        L"应用中卸载“FolderNote 文件夹备注”。备注数据未删除。")};
#endif
      if (!remaining.empty())
        result = 2;
      std::wstring message = remaining.empty()
          ? lang::text(L"已卸载。所有文件夹备注均已保留。")
          : lang::format(L"右键菜单已移除，文件夹备注已保留。以下程序文件仍在使用，请关闭相关窗口或注销后删除：\n{0}",
                         {remaining});
      if (unresolvedContextMenus) {
        message += L"\n\n";
        message += lang::text(L"部分右键菜单已被其他程序修改，原有恢复记录仍保留。");
        result = 2;
      }
      if (unresolvedEntries) {
        message += L"\n\n";
        message += lang::text(L"部分文件夹入口未能还原或移除，处理记录仍保留。");
        result = 2;
      }
      MessageBoxW(nullptr, message.c_str(), L"FolderNote", MB_OK | MB_ICONINFORMATION);
    } else {
      auto source = self.parent_path();
      for (auto name : {L"FolderNote.exe", L"FolderNoteShell.dll", L"FolderNoteNavigation.dll", L"Uninstall.exe"})
        if (GetFileAttributesW((source / name).c_str()) == INVALID_FILE_ATTRIBUTES)
          throw fn::Error{lang::text(L"请完整解压安装包，保持 Install.exe、FolderNote.exe、"
                          L"FolderNoteShell.dll、FolderNoteNavigation.dll 和 Uninstall.exe 在同一文件夹。")};
      navreg::ensureMachine(self);
      auto active=stageVersion(source,dest);
      const auto icon = L"\"" + (active / L"FolderNote.exe").native() + L"\",-" +
                        std::to_wstring(IDI_FOLDERNOTE);
      navigation::stopLegacy();
      navreg::registerUser(active);
      registerMenus(active, navigationOnly);
      context::registerCommands(active / L"FolderNote.exe");
      createSettingsShortcut(active);
      std::wstring u = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstal"
                       L"l\\FolderNote";
      value(u, L"DisplayName", lang::text(L"FolderNote 系统工具"));
      value(u, L"DisplayVersion", app::version);
      value(u, L"DisplayIcon", icon);
      value(u, L"UninstallString", L"\"" + (active / L"Uninstall.exe").native() + L"\"");
      value(u, L"InstallLocation", dest.native());
      SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
      navigation::reload();
#ifndef FOLDERNOTE_UNINSTALL_TEST
      quick::syncHelper((active / L"FolderNote.exe").native());
#endif
      MessageBoxW(nullptr,
                  lang::text(L"安装完成。从开始菜单搜索“FolderNote 设置”调整功能和主题。首次安装默认开启双击返回上一级，备注和中键快速定位默认关闭。覆盖升级保留已有选择、备注、置顶和历史记录。\n\n"
                  L"新版使用独立目录，不受旧文件占用影响。安装器不会注销或重启资源管理器；请关闭并重新打开文件夹窗口以加载新版扩展。\n\n"
                  L"通用版右键菜单在 Windows 11 的“显示更多选项”中；首层菜单请使用 Win11 安装包。"),
                  L"FolderNote", MB_OK | MB_ICONINFORMATION);
    }
  } catch (const fn::Error &e) {
    MessageBoxW(nullptr, e.message.c_str(), lang::text(L"FolderNote 操作未完成"), MB_OK | MB_ICONERROR);
    result = 1;
  } catch (...) {
    MessageBoxW(nullptr, lang::text(L"安装或卸载失败，请检查文件权限。"), L"FolderNote", MB_OK | MB_ICONERROR);
    result = 1;
  }
  LocalFree(args);
  CoUninitialize();
  return result;
}
