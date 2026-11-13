#include "core.h"
#include "language.h"
#include "navigation_registration.h"
#include "window_position.h"
#include <shellapi.h>
#define MessageBoxW ui::message

static DWORD run(const fn::fs::path &program, std::wstring arguments, bool elevated) {
  HANDLE process = nullptr;
  if (elevated) {
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = program.c_str();
    execute.lpParameters = arguments.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute))
      fn::fail(L"管理员授权未完成");
    process = execute.hProcess;
  } else {
    auto command = L"\"" + program.native() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &information))
      fn::fail(L"无法启动安装");
    process = information.hProcess;
    CloseHandle(information.hThread);
  }
  if (!process)
    throw fn::Error{lang::text(L"无法确认安装结果。")};
  WaitForSingleObject(process, INFINITE);
  DWORD result = 1;
  GetExitCodeProcess(process, &result);
  CloseHandle(process);
  return result;
}

static int installPackage(const std::wstring &originalSid) {
  // Add-AppxPackage is per-user. Refuse alternate administrator credentials
  // rather than silently installing the package for the administrator account.
  if (!navreg::validSid(originalSid) || navreg::currentUserSid() != originalSid)
    return 4;
  auto package = fn::modulePath().parent_path() / L"FolderNote.msix";
  if (GetFileAttributesW(package.c_str()) == INVALID_FILE_ATTRIBUTES)
    throw fn::Error{lang::text(L"未找到 FolderNote.msix。")};
  std::wstring escaped;
  for (auto character : package.native()) {
    escaped += character;
    if (character == L'\'')
      escaped += character;
  }
  wchar_t system[MAX_PATH];
  GetSystemDirectoryW(system, MAX_PATH);
  auto powershell = fn::fs::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
  auto command = L"-NoProfile -NonInteractive -Command \"try { Add-AppxPackage -Path '" +
                 escaped + L"' -AllowUnsigned -ErrorAction Stop; exit 0 } catch { exit 1 }\"";
  if (run(powershell, command, false))
    return 1;
  navreg::registerMachine(originalSid);
  return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  int count = 0;
  auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  const bool worker = count == 3 && std::wstring(arguments[1]) == L"--install-package";
  DWORD parent = 0;
  if (count == 3 && std::wstring(arguments[1]) == L"--parent") {
    wchar_t *end = nullptr;
    auto id = wcstoul(arguments[2], &end, 10);
    if (end && !*end) parent = id;
  }
  std::wstring sid = worker ? arguments[2] : L"";
  LocalFree(arguments);
  try {
    if (worker)
      return installPackage(sid);
    if (parent && parent != GetCurrentProcessId()) {
      // The settings app may be running from the MSIX being replaced.
      HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, parent);
      if (process) {
        const auto wait = WaitForSingleObject(process, 15000);
        CloseHandle(process);
        if (wait != WAIT_OBJECT_0) throw fn::Error{lang::text(L"请关闭 FolderNote 设置后再安装更新。")};
      }
    }
    auto source = fn::modulePath().parent_path();
    for (auto name : {L"FolderNote.msix", L"Install.exe", L"FolderNote.exe",
                      L"FolderNoteShell.dll", L"FolderNoteNavigation.dll", L"Uninstall.exe"})
      if (GetFileAttributesW((source / name).c_str()) == INVALID_FILE_ATTRIBUTES)
        throw fn::Error{lang::text(L"请完整解压 Win11 安装包，并保持所有程序文件在同一文件夹。")};
    sid = navreg::currentUserSid();
    auto result = run(fn::modulePath(), L"--install-package " + sid, true);
    if (result == 4)
      throw fn::Error{lang::text(L"Win11 应用包必须安装到当前账户。请使用当前账户的管理员授权；"
                      L"不能用另一管理员账户的凭据代替。通用版安装器支持另一管理员授权。")};
    if (result)
      throw fn::Error{lang::text(L"安装未成功。需要 Windows 11；组织策略也可能禁止未签名测试包。"
                      L"未修改系统安全设置。")};
    // Keep user file staging and COM registration outside the elevated worker.
    if (run(source / L"Install.exe", L"--navigation-only --quiet", false))
      throw fn::Error{lang::text(L"Win11 菜单包已安装，但双击导航组件安装未完成。请重新运行安装器。")};
    MessageBoxW(nullptr,
                lang::text(L"FolderNote 系统工具已安装，备注、双击导航和中键快速定位可独立开启或关闭；支持浅色、深色和跟随系统。\n\n"
                L"无需注销；关闭并重新打开文件夹窗口后，导航扩展会自动加载。"
                L"在开始菜单搜索“FolderNote 设置”。首次安装默认开启双击返回上一级，备注和中键快速定位默认关闭；开启备注后才显示相关右键菜单。\n\n"
                L"覆盖升级保留已有选择。若旧版开启后仍无反应，请重新保存一次双击导航设置。\n\n"
                L"菜单排序由 Windows 决定。"),
                L"FolderNote", MB_OK);
    return 0;
  } catch (const fn::Error &error) {
    if (!worker)
      MessageBoxW(nullptr, error.message.c_str(), lang::text(L"FolderNote 安装未完成"), MB_OK | MB_ICONERROR);
    return 1;
  } catch (...) {
    return 1;
  }
}
