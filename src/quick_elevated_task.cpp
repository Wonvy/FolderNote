#include "quick_folders.h"
#include "navigation.h"
#include "core.h"
#include "language.h"
#include "runtime_guard.h"
#include <sddl.h>
#include <shellapi.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <cwchar>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace quick {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t taskFolderPath[] = L"\\ShibaKit";
constexpr wchar_t taskFolderName[] = L"ShibaKit";
constexpr wchar_t taskNamePrefix[] = L"ElevatedDialogHelper-";

struct Bstr {
  BSTR value = nullptr;
  explicit Bstr(const wchar_t *text) : value(text ? SysAllocString(text) : nullptr) {}
  explicit Bstr(const std::wstring &text) : value(SysAllocString(text.c_str())) {}
  ~Bstr() { SysFreeString(value); }
  Bstr(const Bstr &) = delete;
  Bstr &operator=(const Bstr &) = delete;
  BSTR get() const noexcept { return value; }
};

struct Variant {
  VARIANT value{};
  Variant() { VariantInit(&value); }
  ~Variant() { VariantClear(&value); }
  Variant(const Variant &) = delete;
  Variant &operator=(const Variant &) = delete;
  VARIANT get() const noexcept { return value; }
};

struct CoInit {
  HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool owns = SUCCEEDED(result);
  bool ok = owns || result == RPC_E_CHANGED_MODE;
  ~CoInit() { if (owns) CoUninitialize(); }
};

bool validSid(const std::wstring &sid) {
  if (sid.empty() || sid.size() > 184 || sid.find_first_not_of(L"S-0123456789") != std::wstring::npos)
    return false;
  PSID binary = nullptr;
  if (!ConvertStringSidToSidW(sid.c_str(), &binary)) return false;
  LPWSTR canonical = nullptr;
  const bool ok = IsValidSid(binary) && ConvertSidToStringSidW(binary, &canonical) && sid == canonical;
  if (canonical) LocalFree(canonical);
  LocalFree(binary);
  return ok;
}

std::wstring currentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<BYTE> data(size);
  const BOOL read = size && GetTokenInformation(token, TokenUser, data.data(), size, &size);
  CloseHandle(token);
  if (!read) return {};
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(data.data())->User.Sid, &sid) || !sid)
    return {};
  std::wstring result(sid);
  LocalFree(sid);
  return result;
}

bool currentProcessElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD size = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  std::vector<BYTE> data(size);
  const BOOL read = size && GetTokenInformation(token, TokenIntegrityLevel, data.data(), size, &size);
  CloseHandle(token);
  if (!read) return false;
  auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(data.data())->Label.Sid;
  return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1) >= SECURITY_MANDATORY_HIGH_RID;
}

std::wstring taskNameForSid(const std::wstring &sid) {
  return std::wstring(taskNamePrefix) + sid;
}

HRESULT connectScheduler(ComPtr<ITaskService> &service) {
  HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
  if (FAILED(hr) || !service) return FAILED(hr) ? hr : E_FAIL;
  Variant empty;
  return service->Connect(empty.get(), empty.get(), empty.get(), empty.get());
}

void installElevatedTask(const std::wstring &sid, const std::wstring &exePath) {
  const DWORD attrs = GetFileAttributesW(exePath.c_str());
  if (exePath.empty() || attrs == INVALID_FILE_ATTRIBUTES ||
      (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    fn::fail(lang::text(L"未找到已安装的 ShibaKit，请重新安装后再试。"), ERROR_FILE_NOT_FOUND);
  }
  CoInit com;
  if (!com.ok) fn::fail(lang::text(L"无法配置管理员文件对话框计划任务"), static_cast<DWORD>(com.result));
  ComPtr<ITaskService> service;
  HRESULT hr = connectScheduler(service);
  if (FAILED(hr)) fn::fail(lang::text(L"无法连接系统计划任务服务"), static_cast<DWORD>(hr));

  Variant empty;
  ComPtr<ITaskFolder> rootFolder, folder;
  hr = service->GetFolder(Bstr(L"\\").get(), &rootFolder);
  if (FAILED(hr) || !rootFolder) fn::fail(lang::text(L"无法打开系统计划任务根目录"), static_cast<DWORD>(hr));
  hr = service->GetFolder(Bstr(taskFolderPath).get(), &folder);
  if (FAILED(hr) || !folder) {
    hr = rootFolder->CreateFolder(Bstr(taskFolderName).get(), empty.get(), &folder);
  }
  if (FAILED(hr) || !folder) fn::fail(lang::text(L"无法创建 ShibaKit 计划任务目录"), static_cast<DWORD>(hr));

  ComPtr<ITaskDefinition> definition;
  hr = service->NewTask(0, &definition);
  if (FAILED(hr) || !definition) fn::fail(lang::text(L"无法创建管理员文件对话框计划任务"), static_cast<DWORD>(hr));

  ComPtr<IRegistrationInfo> regInfo;
  if (SUCCEEDED(definition->get_RegistrationInfo(&regInfo)) && regInfo) {
    regInfo->put_Author(Bstr(L"ShibaKit").get());
    regInfo->put_Description(Bstr(L"ShibaKit Elevated File Dialog Helper").get());
  }

  ComPtr<IPrincipal> principal;
  hr = definition->get_Principal(&principal);
  if (FAILED(hr) || !principal ||
      FAILED(hr = principal->put_UserId(Bstr(sid).get())) ||
      FAILED(hr = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
      FAILED(hr = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST))) {
    fn::fail(lang::text(L"无法设置计划任务运行权限"), static_cast<DWORD>(hr));
  }

  ComPtr<ITaskSettings> settings;
  hr = definition->get_Settings(&settings);
  if (FAILED(hr) || !settings ||
      FAILED(hr = settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
      FAILED(hr = settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
      FAILED(hr = settings->put_ExecutionTimeLimit(Bstr(L"PT0S").get())) ||
      FAILED(hr = settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
      FAILED(hr = settings->put_StartWhenAvailable(VARIANT_FALSE))) {
    fn::fail(lang::text(L"无法配置计划任务电源和实例选项"), static_cast<DWORD>(hr));
  }

  ComPtr<ITriggerCollection> triggers;
  ComPtr<ITrigger> trigger;
  ComPtr<ILogonTrigger> logonTrigger;
  hr = definition->get_Triggers(&triggers);
  if (FAILED(hr) || !triggers ||
      FAILED(hr = triggers->Create(TASK_TRIGGER_LOGON, &trigger)) || !trigger ||
      FAILED(hr = trigger.As(&logonTrigger)) || !logonTrigger ||
      FAILED(hr = logonTrigger->put_UserId(Bstr(sid).get())) ||
      FAILED(hr = logonTrigger->put_Enabled(VARIANT_TRUE))) {
    fn::fail(lang::text(L"无法创建登录触发器"), static_cast<DWORD>(hr));
  }

  ComPtr<IActionCollection> actions;
  ComPtr<IAction> action;
  ComPtr<IExecAction> execAction;
  hr = definition->get_Actions(&actions);
  if (FAILED(hr) || !actions ||
      FAILED(hr = actions->Create(TASK_ACTION_EXEC, &action)) || !action ||
      FAILED(hr = action.As(&execAction)) || !execAction ||
      FAILED(hr = execAction->put_Path(Bstr(exePath).get())) ||
      FAILED(hr = execAction->put_Arguments(Bstr(L"--quick-elevated-helper").get()))) {
    fn::fail(lang::text(L"无法配置计划任务启动命令"), static_cast<DWORD>(hr));
  }

  ComPtr<IRegisteredTask> registered;
  hr = folder->RegisterTaskDefinition(Bstr(taskNameForSid(sid)).get(), definition.Get(),
      TASK_CREATE_OR_UPDATE, empty.get(), empty.get(), TASK_LOGON_INTERACTIVE_TOKEN, empty.get(), &registered);
  if (FAILED(hr) || !registered)
    fn::fail(lang::text(L"无法注册管理员文件对话框计划任务"), static_cast<DWORD>(hr));
}

void removeElevatedTask(const std::wstring &sid) {
  CoInit com;
  if (!com.ok) fn::fail(lang::text(L"无法连接系统计划任务服务"), static_cast<DWORD>(com.result));
  ComPtr<ITaskService> service;
  HRESULT hr = connectScheduler(service);
  if (FAILED(hr)) fn::fail(lang::text(L"无法连接系统计划任务服务"), static_cast<DWORD>(hr));
  ComPtr<ITaskFolder> folder;
  hr = service->GetFolder(Bstr(taskFolderPath).get(), &folder);
  if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return;
  if (FAILED(hr) || !folder) fn::fail(lang::text(L"无法打开 ShibaKit 计划任务目录"), static_cast<DWORD>(hr));
  hr = folder->DeleteTask(Bstr(taskNameForSid(sid)).get(), 0);
  if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
      hr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
    fn::fail(lang::text(L"无法移除管理员文件对话框计划任务"), static_cast<DWORD>(hr));
  }
  ComPtr<ITaskFolder> rootFolder;
  if (SUCCEEDED(service->GetFolder(Bstr(L"\\").get(), &rootFolder)) && rootFolder) {
    rootFolder->DeleteFolder(Bstr(taskFolderName).get(), 0);
  }
}

bool launchElevatedDaemon(const std::wstring &exePath) {
  auto command = L"\"" + exePath + L"\" --quick-elevated-helper";
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (elevatedHelperRunning()) return true;
    Sleep(50);
  }
  return false;
}

void applyElevatedStateDirect(const std::wstring &sid, const std::wstring &exePath, bool enable) {
  if (enable) {
    navigation::setElevatedDialogQuickEnabled(true);
    stopElevatedHelper();
    installElevatedTask(sid, exePath);
    if (!launchElevatedDaemon(exePath))
      fn::fail(lang::text(L"选项已保存，但无法启动管理员文件对话框后台组件"), ERROR_TIMEOUT);
  } else {
    navigation::setElevatedDialogQuickEnabled(false);
    stopElevatedHelper();
    removeElevatedTask(sid);
  }
}
} // namespace

bool elevatedTaskInstalled() {
  try {
    const auto sid = currentUserSid();
    if (!validSid(sid)) return false;
    CoInit com;
    if (!com.ok) return navigation::elevatedDialogQuickEnabled() || elevatedHelperRunning();
    ComPtr<ITaskService> service;
    HRESULT hr = connectScheduler(service);
    if (FAILED(hr)) return navigation::elevatedDialogQuickEnabled() || elevatedHelperRunning();
    ComPtr<ITaskFolder> folder;
    hr = service->GetFolder(Bstr(taskFolderPath).get(), &folder);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
      return false;
    if (FAILED(hr) || !folder) return navigation::elevatedDialogQuickEnabled() || elevatedHelperRunning();
    ComPtr<IRegisteredTask> task;
    hr = folder->GetTask(Bstr(taskNameForSid(sid)).get(), &task);
    if (SUCCEEDED(hr) && task) return true;
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
      return false;
    return navigation::elevatedDialogQuickEnabled() || elevatedHelperRunning();
  } catch (...) {
    return navigation::elevatedDialogQuickEnabled() || elevatedHelperRunning();
  }
}

bool handleElevatedConfigureCommand(int argc, wchar_t **argv, int &exitCode) {
  if (argc < 2 || std::wstring_view(argv[1]) != L"--configure-elevated-dialog") return false;
  exitCode = 1;
  if (argc != 4) return true;
  const std::wstring_view action(argv[2]);
  const std::wstring sid(argv[3]);
  if ((action != L"install" && action != L"remove") || !validSid(sid)) return true;
  if (!currentProcessElevated()) { exitCode = 3; return true; }
  const auto ownSid = currentUserSid();
  if (!validSid(ownSid) || ownSid != sid) { exitCode = 2; return true; }
  try {
    applyElevatedStateDirect(sid, fn::modulePath().native(), action == L"install");
    exitCode = 0;
  } catch (const fn::Error &error) {
    guard::log(L"配置管理员文件对话框组件失败：" + error.message);
  } catch (...) {
    guard::log(L"配置管理员文件对话框组件出现未知异常");
  }
  return true;
}

bool configureElevatedHelper(const std::wstring &exe, bool enable) {
  const auto sid = currentUserSid();
  if (!validSid(sid)) throw fn::Error{lang::text(L"无法读取用户信息")};
  if (currentProcessElevated()) {
    applyElevatedStateDirect(sid, exe, enable);
    return true;
  }
  std::wstring params = enable ? L"--configure-elevated-dialog install " : L"--configure-elevated-dialog remove ";
  params += sid;
  SHELLEXECUTEINFOW execute{sizeof(execute)};
  execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  execute.lpVerb = L"runas";
  execute.lpFile = exe.c_str();
  execute.lpParameters = params.c_str();
  execute.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execute)) {
    const DWORD err = GetLastError();
    if (err == ERROR_CANCELLED) return false;
    fn::fail(lang::text(L"管理员文件对话框支持需要管理员授权"), err);
  }
  if (!execute.hProcess)
    throw fn::Error{lang::text(L"无法确认管理员文件对话框支持配置结果。")};
  WaitForSingleObject(execute.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(execute.hProcess, &code);
  CloseHandle(execute.hProcess);
  if (code == 2)
    throw fn::Error{lang::text(L"请使用当前登录用户的管理员账户授权，不支持跨账户提权。")};
  if (code != 0) {
    throw fn::Error{lang::text(enable ? L"未能完成管理员文件对话框支持的计划任务配置或后台启动。"
                                      : L"未能移除管理员文件对话框支持的计划任务。")};
  }
  navigation::setElevatedDialogQuickEnabled(enable);
  return true;
}

void syncElevatedHelper(const std::wstring &exe, bool allowElevation) {
  const bool want = navigation::elevatedDialogQuickEnabled();
  const bool running = elevatedHelperRunning();
  const bool installed = elevatedTaskInstalled();
  if (!want && !running && !installed) return;
  if (want) {
    if (allowElevation) configureElevatedHelper(exe, true);
  } else {
    signalElevatedHelperStop();
    if (allowElevation) configureElevatedHelper(exe, false);
  }
}

void removeElevatedHelper(const std::wstring &exe) {
  const bool hadState = navigation::elevatedDialogQuickEnabled() || elevatedHelperRunning() || elevatedTaskInstalled();
  if (!hadState) return;
  navigation::setElevatedDialogQuickEnabled(false);
  signalElevatedHelperStop();
  const DWORD attrs = GetFileAttributesW(exe.c_str());
  const bool exeAvailable = !exe.empty() && attrs != INVALID_FILE_ATTRIBUTES &&
                            !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
  if (exeAvailable) {
    configureElevatedHelper(exe, false);
  } else if (elevatedTaskInstalled()) {
    throw fn::Error{lang::text(L"无法定位 ShibaKit 主程序来移除管理员文件对话框计划任务。")};
  }
}
} // namespace quick
