#include "desktop_groups_process.h"
#include <ole2.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <optional>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

struct ProcessIdentity {
  DWORD integrity = 0;
  alignas(SID) std::array<BYTE, SECURITY_MAX_SID_SIZE> user{};
};

std::optional<ProcessIdentity> processIdentity(DWORD pid) {
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), CloseHandle);
  HANDLE rawToken = nullptr;
  if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken)) return {};
  Handle token(rawToken, CloseHandle);
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
  alignas(TOKEN_MANDATORY_LABEL) BYTE label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
  DWORD size = 0;
  if (!GetTokenInformation(token.get(), TokenUser, user, sizeof(user), &size) ||
      !GetTokenInformation(token.get(), TokenIntegrityLevel, label, sizeof(label), &size)) return {};
  const auto integritySid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(label)->Label.Sid;
  const auto count = *GetSidSubAuthorityCount(integritySid);
  ProcessIdentity result;
  if (!count || !CopySid(static_cast<DWORD>(result.user.size()), result.user.data(),
                         reinterpret_cast<TOKEN_USER *>(user)->User.Sid)) return {};
  result.integrity = *GetSidSubAuthority(integritySid, count - 1);
  return result;
}
}

DesktopLaunchContext desktopLaunchContext() {
  auto current = processIdentity(GetCurrentProcessId());
  if (!current) return {};
  DWORD pid = 0;
  if (const auto window = GetShellWindow()) GetWindowThreadProcessId(window, &pid);
  auto shell = pid ? processIdentity(pid) : std::nullopt;
  if (!shell) {
    const auto mode = current->integrity < SECURITY_MANDATORY_HIGH_RID ? DesktopLaunchMode::Direct :
                                                                        DesktopLaunchMode::Unavailable;
    return {mode, 0, current->integrity};
  }
  if (current->integrity <= shell->integrity) return {DesktopLaunchMode::Direct, pid, shell->integrity};
  if (!EqualSid(current->user.data(), shell->user.data())) return {};
  return {DesktopLaunchMode::Explorer, pid, shell->integrity};
}

DesktopReceiverStatus desktopReceiverStatus(HWND window, const DesktopLaunchContext &context) {
  if (!window || context.mode == DesktopLaunchMode::Unavailable) return DesktopReceiverStatus::Unavailable;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  auto receiver = pid ? processIdentity(pid) : std::nullopt;
  auto shell = processIdentity(context.shellPid ? context.shellPid : GetCurrentProcessId());
  if (!receiver || !shell || !EqualSid(receiver->user.data(), shell->user.data()))
    return DesktopReceiverStatus::Unavailable;
  return receiver->integrity > context.shellIntegrity ? DesktopReceiverStatus::Elevated :
                                                       DesktopReceiverStatus::Compatible;
}

bool executeFromDesktop(const std::wstring &exe, const wchar_t *arguments, const DesktopLaunchContext &context) {
  if (context.mode != DesktopLaunchMode::Explorer) return false;
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) return false;
  VARIANT location{}, root{};
  location.vt = VT_I4;
  location.lVal = CSIDL_DESKTOP;
  long handle = 0;
  ComPtr<IDispatch> desktop;
  ComPtr<IServiceProvider> services;
  ComPtr<IShellBrowser> browser;
  ComPtr<IShellView> view;
  if (windows->FindWindowSW(&location, &root, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, &desktop) != S_OK ||
      !desktop || FAILED(desktop.As(&services)) ||
      FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser ||
      FAILED(browser->QueryActiveShellView(&view)) || !view) return false;
  HWND viewWindow = nullptr;
  DWORD viewPid = 0;
  if (FAILED(view->GetWindow(&viewWindow)) || !GetWindowThreadProcessId(viewWindow, &viewPid) ||
      viewPid != context.shellPid) return false;
  ComPtr<IDispatch> background, application;
  ComPtr<IShellFolderViewDual> folder;
  ComPtr<IShellDispatch2> shell;
  if (FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background))) ||
      !background || FAILED(background.As(&folder)) ||
      FAILED(folder->get_Application(&application)) || !application || FAILED(application.As(&shell))) return false;

  using String = std::unique_ptr<OLECHAR, decltype(&SysFreeString)>;
  const String file(SysAllocString(exe.c_str()), SysFreeString);
  const String parameters(SysAllocString(arguments), SysFreeString);
  if (!file || !parameters) return false;
  VARIANT args{}, empty{}, show{};
  args.vt = VT_BSTR;
  args.bstrVal = parameters.get();
  show.vt = VT_I4;
  show.lVal = SW_SHOWNORMAL;
  return SUCCEEDED(shell->ShellExecute(file.get(), args, empty, empty, show));
}
}
