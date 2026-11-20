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
  auto modifierPressed = [] {
    return (GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU)) & 0x8000;
  };
  if (modifierPressed()) return NavigateResult::ModifierPressed;
  const HWND address = addressControl(target);
  HWND edit = nullptr;
  auto waitForAddress = [&]() {
    for (int i = 0; i < 50; ++i) {
      if (!current() || GetForegroundWindow() != target) return NavigateResult::TargetChanged;
      GUITHREADINFO info{sizeof(info)};
      if (GetGUIThreadInfo(thread, &info) && addressEdit(info.hwndFocus, target) &&
          IsWindowVisible(info.hwndFocus)) {
        edit = info.hwndFocus;
        return NavigateResult::Success;
      }
      Sleep(10);
    }
    return NavigateResult::AddressUnavailable;
  };
  if (address && IsWindowVisible(address)) {
    if (!SendMessageTimeoutW(target, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(address), TRUE,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result)) return NavigateResult::InputBlocked;
    if (waitForAddress() == NavigateResult::TargetChanged) return NavigateResult::TargetChanged;
  }
  if (!edit) {
    // Breadcrumb mode can create the edit on demand. Queue the address-bar
    // accelerator on the dialog so its message loop can translate it locally.
    if (!current() || GetForegroundWindow() != target) return NavigateResult::TargetChanged;
    if (modifierPressed()) return NavigateResult::ModifierPressed;
    const LPARAM key = 1 | (MapVirtualKeyW(VK_F4, MAPVK_VK_TO_VSC) << 16);
    if (!PostMessageW(target, WM_KEYDOWN, VK_F4, key) ||
        !PostMessageW(target, WM_KEYUP, VK_F4, key | 0xc0000000)) return NavigateResult::InputBlocked;
    const auto focused = waitForAddress();
    if (focused != NavigateResult::Success) return focused;
  }
  auto editHasFocus = [&] {
    for (int i = 0; i < 50; ++i) {
      GUITHREADINFO info{sizeof(info)};
      if (!current() || GetForegroundWindow() != target || !GetGUIThreadInfo(thread, &info) ||
          !addressEdit(edit, target) || !IsWindowVisible(edit)) return false;
      if (info.hwndFocus == edit) return true;
      // While handling a cross-process WM_SETTEXT, the address bar can briefly
      // give focus to its combo box. Wait only for this same edit to regain it.
      wchar_t cls[32]{};
      if (!info.hwndFocus || info.hwndFocus != GetParent(edit) ||
          !GetClassNameW(info.hwndFocus, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"ComboBox")) return false;
      Sleep(10);
    }
    return false;
  };
  if (!editHasFocus()) return NavigateResult::TargetChanged;
  if (!SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()),
      SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) || !result) return NavigateResult::AddressWriteFailed;
  if (!editHasFocus()) return NavigateResult::TargetChanged;
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
