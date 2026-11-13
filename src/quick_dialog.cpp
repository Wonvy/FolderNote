#include "quick_dialog.h"
#include "language.h"
#include "quick_folders.h"

namespace quick::dialog {
namespace {
HWND addressControl(HWND target) {
  HWND found = nullptr;
  struct Search { HWND target; HWND *found; } search{target, &found};
  EnumChildWindows(target, [](HWND child, LPARAM data) -> BOOL {
    auto &search = *reinterpret_cast<Search *>(data);
    if (addressEdit(child, search.target)) {
      *search.found = child; return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return found;
}
}
NavigateResult navigate(HWND target, const std::wstring &path, const detail::MenuRequest &request) {
  DWORD pid = 0;
  const DWORD thread = GetWindowThreadProcessId(target, &pid);
  auto current = [&] {
    DWORD currentPid = 0;
    return request.current() && thread && pid &&
      GetWindowThreadProcessId(target, &currentPid) == thread && currentPid == pid && isFileDialog(target);
  };
  if (!current()) return NavigateResult::TargetChanged;
  SetForegroundWindow(target);
  // Activation of another input queue is asynchronous. A bounded, harmless
  // message lets the dialog process that activation before we inspect focus.
  DWORD_PTR result = 0;
  if (!SendMessageTimeoutW(target, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result))
    return NavigateResult::ActivationFailed;
  if (!current()) return NavigateResult::TargetChanged;
  if (GetForegroundWindow() != target) return NavigateResult::ActivationFailed;
  if ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU)) & 0x8000)
    return NavigateResult::ModifierPressed;
  // The native address edit also exists in breadcrumb mode. Ask its dialog to
  // focus it directly: global Alt+D can be intercepted by another application's
  // hotkey, taking focus away before any path can be entered.
  const HWND address = addressControl(target);
  if (!address) return NavigateResult::AddressUnavailable;
  if (!SendMessageTimeoutW(target, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(address), TRUE,
      SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result))
    return NavigateResult::InputBlocked;
  HWND edit = nullptr;
  for (int i = 0; i < 50; ++i) {
    if (!current() || GetForegroundWindow() != target) return NavigateResult::TargetChanged;
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(thread, &info) && addressEdit(info.hwndFocus, target)) {
      edit = info.hwndFocus; break;
    }
    Sleep(10);
  }
  if (!edit) return NavigateResult::AddressUnavailable;
  GUITHREADINFO info{sizeof(info)};
  if (!current() || GetForegroundWindow() != target || !GetGUIThreadInfo(thread, &info) ||
      info.hwndFocus != edit || !addressEdit(edit, target)) return NavigateResult::TargetChanged;
  if (!SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()),
      SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) || !result) return NavigateResult::AddressWriteFailed;
  info = {sizeof(info)};
  if (!current() || GetForegroundWindow() != target || !GetGUIThreadInfo(thread, &info) ||
      info.hwndFocus != edit || !addressEdit(edit, target)) return NavigateResult::TargetChanged;
  // Enter goes only to the verified address edit, never a filename control,
  // the dialog's default button, or a different foreground application.
  if (!SendMessageTimeoutW(edit, WM_KEYDOWN, VK_RETURN, 1,
      SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result)) return NavigateResult::AddressSubmitFailed;
  SendMessageTimeoutW(edit, WM_KEYUP, VK_RETURN, 0xc0000001, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
  return NavigateResult::Success;
}

const wchar_t *failureMessage(NavigateResult result) noexcept {
  switch (result) {
    case NavigateResult::TargetChanged: return lang::text(L"原对话框或焦点已经改变，已取消定位。请回到原对话框后重试。");
    case NavigateResult::ActivationFailed: return lang::text(L"未能激活原文件对话框。请回到该对话框后重试。");
    case NavigateResult::ModifierPressed: return lang::text(L"请松开 Ctrl、Shift 和 Alt 键后重试文件夹定位。");
    case NavigateResult::InputBlocked: return lang::text(L"无法操作文件对话框的地址栏。请确认目标程序未以管理员权限运行。");
    case NavigateResult::AddressUnavailable: return lang::text(L"未能找到文件对话框的地址栏。不支持的自定义对话框可使用“在新窗口打开”。");
    case NavigateResult::AddressWriteFailed: return lang::text(L"无法填写文件对话框的地址栏。请确认目标程序未以管理员权限运行。");
    case NavigateResult::AddressSubmitFailed: return lang::text(L"文件对话框未响应地址栏定位，请稍后重试。");
    default: return lang::text(L"未能在原文件对话框定位文件夹。");
  }
}
}
