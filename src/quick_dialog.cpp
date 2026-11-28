#include "quick_dialog.h"
#include "language.h"
#include "quick_folders.h"
#include <algorithm>
#include <cwchar>
#include <optional>
#include <vector>

namespace quick::dialog {
namespace {
bool classIs(HWND window, const wchar_t *name) {
  wchar_t cls[128]{};
  return window && GetClassNameW(window, cls, ARRAYSIZE(cls)) && !wcscmp(cls, name);
}

std::optional<std::wstring> readEditText(HWND edit) {
  DWORD_PTR length = 0;
  if (!SendMessageTimeoutW(edit, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &length))
    return std::nullopt;
  if (length > 4096) return std::nullopt;
  std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
  DWORD_PTR copied = 0;
  if (!SendMessageTimeoutW(edit, WM_GETTEXT, buffer.size(), reinterpret_cast<LPARAM>(buffer.data()),
                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &copied)) {
    return std::nullopt;
  }
  buffer.resize(wcsnlen(buffer.c_str(), buffer.size()));
  return buffer;
}

bool hasVistaAddressHost(HWND target) {
  bool found = false;
  EnumChildWindows(target, [](HWND child, LPARAM data) -> BOOL {
    wchar_t cls[128]{};
    if (GetClassNameW(child, cls, ARRAYSIZE(cls)) &&
        (!wcscmp(cls, L"Address Band Root") || !wcscmp(cls, L"FileNameControlHost") || wcsstr(cls, L"Breadcrumb"))) {
      *reinterpret_cast<bool *>(data) = true;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&found));
  return found;
}

bool excludedEditAncestor(HWND control, HWND target) {
  for (HWND parent = GetParent(control); parent && parent != target; parent = GetParent(parent)) {
    wchar_t cls[128]{};
    if (!GetClassNameW(parent, cls, ARRAYSIZE(cls))) continue;
    if (!wcscmp(cls, L"SHELLDLL_DefView") || !wcscmp(cls, L"UniversalSearchBand") ||
        !wcscmp(cls, L"DirectUIHWND") || wcsstr(cls, L"Breadcrumb") || wcsstr(cls, L"Address Band")) {
      return true;
    }
  }
  return false;
}

struct ClassicControls {
  HWND edit = nullptr;
  HWND okButton = nullptr;
};

ClassicControls findClassicControls(HWND target) {
  if (hasVistaAddressHost(target)) return {};
  struct Search {
    HWND target;
    bool hasShellView = false;
    HWND idOkButton = nullptr;
    HWND fallbackDefButton = nullptr;
    std::vector<HWND> candidates;
  } search{target};
  EnumChildWindows(target, [](HWND child, LPARAM data) -> BOOL {
    auto &s = *reinterpret_cast<Search *>(data);
    wchar_t cls[128]{};
    if (!GetClassNameW(child, cls, ARRAYSIZE(cls))) return TRUE;
    if (!wcscmp(cls, L"SHELLDLL_DefView")) s.hasShellView = true;
    if (!wcscmp(cls, L"Button") && IsWindowVisible(child) && IsWindowEnabled(child)) {
      const int id = GetDlgCtrlID(child);
      if (id == IDOK) {
        if (!s.idOkButton) s.idOkButton = child;
      } else if (!s.fallbackDefButton && GetParent(child) == s.target &&
                 (GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_DEFPUSHBUTTON) {
        s.fallbackDefButton = child;
      }
    }
    if (!wcscmp(cls, L"Edit") && IsWindowVisible(child) && IsWindowEnabled(child)) {
      const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
      if (!(style & ES_MULTILINE) && !(style & ES_READONLY) && !excludedEditAncestor(child, s.target))
        s.candidates.push_back(child);
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));

  HWND okButton = search.idOkButton ? search.idOkButton : search.fallbackDefButton;
  if (!search.hasShellView || !okButton || search.candidates.empty()) return {};

  HWND selectedEdit = nullptr;
  for (HWND candidate : search.candidates) {
    const int id = GetDlgCtrlID(candidate);
    const HWND parent = GetParent(candidate);
    const HWND grand = parent ? GetParent(parent) : nullptr;
    if (id == 1152 ||
        (parent && parent != target && classIs(parent, L"ComboBox") && GetDlgCtrlID(parent) == 1148) ||
        (grand && grand != target && classIs(grand, L"ComboBoxEx32") && GetDlgCtrlID(grand) == 1148)) {
      selectedEdit = candidate;
      break;
    }
  }
  if (!selectedEdit && search.candidates.size() == 1) {
    selectedEdit = search.candidates[0];
  }
  if (!selectedEdit) {
    RECT dialogRect{};
    if (GetWindowRect(target, &dialogRect)) {
      const LONG midY = (dialogRect.top + dialogRect.bottom) / 2;
      std::vector<HWND> bottomCombos;
      for (HWND candidate : search.candidates) {
        const HWND parent = GetParent(candidate);
        RECT editRect{};
        if (parent && parent != target && classIs(parent, L"ComboBox") &&
            GetWindowRect(candidate, &editRect) && editRect.top >= midY) {
          bottomCombos.push_back(candidate);
        }
      }
      if (bottomCombos.size() == 1) selectedEdit = bottomCombos[0];
    }
  }
  if (!selectedEdit) return {};
  return {selectedEdit, okButton};
}

template <class CurrentPred, class ModifierPred>
NavigateResult navigateClassic(HWND target, ClassicControls controls, const std::wstring &path, DWORD thread,
                               const CurrentPred &current, const ModifierPred &modifierPressed) {
  const DWORD attrs = GetFileAttributesW(path.c_str());
  if (path.empty() || attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    return NavigateResult::AddressWriteFailed;

  std::wstring dirPath = path;
  if (dirPath.back() != L'\\') dirPath.push_back(L'\\');

  if (!current() || GetForegroundWindow() != target) return NavigateResult::TargetChanged;
  if (!IsWindowVisible(controls.edit) || !IsWindowEnabled(controls.edit)) return NavigateResult::AddressUnavailable;
  if (modifierPressed()) return NavigateResult::ModifierPressed;

  std::wstring originalName;
  if (const auto existing = readEditText(controls.edit); existing && !existing->empty() &&
      existing->find(L'\\') == std::wstring::npos && existing->find(L'/') == std::wstring::npos) {
    originalName = *existing;
  }

  DWORD_PTR result = 0;
  if (!SendMessageTimeoutW(target, WM_NEXTDLGCTL, reinterpret_cast<WPARAM>(controls.edit), TRUE,
                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result)) {
    return NavigateResult::InputBlocked;
  }

  const HWND parent = GetParent(controls.edit);
  bool focused = false;
  for (int i = 0; i < 30; ++i) {
    if (!current() || GetForegroundWindow() != target) return NavigateResult::TargetChanged;
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(thread, &info) &&
        (info.hwndFocus == controls.edit ||
         (parent && parent != target && classIs(parent, L"ComboBox") && info.hwndFocus == parent))) {
      focused = true;
      break;
    }
    Sleep(10);
  }
  if (!focused) return NavigateResult::TargetChanged;
  if (!current() || GetForegroundWindow() != target ||
      !IsWindowVisible(controls.edit) || !IsWindowEnabled(controls.edit)) {
    return NavigateResult::TargetChanged;
  }

  if (!SendMessageTimeoutW(controls.edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(dirPath.c_str()),
                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) || !result) {
    return NavigateResult::AddressWriteFailed;
  }

  const auto restoreOriginal = [&] {
    if (!originalName.empty() && current() && GetForegroundWindow() == target && IsWindow(controls.edit)) {
      DWORD_PTR ignored = 0;
      SendMessageTimeoutW(controls.edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(originalName.c_str()),
                          SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored);
    }
  };

  const auto readback = readEditText(controls.edit);
  if (!readback || *readback != dirPath) {
    restoreOriginal();
    return NavigateResult::AddressWriteFailed;
  }
  if (modifierPressed()) {
    restoreOriginal();
    return NavigateResult::ModifierPressed;
  }
  if (!current() || GetForegroundWindow() != target) return NavigateResult::TargetChanged;

  if (!PostMessageW(controls.edit, WM_KEYDOWN, VK_RETURN, 1)) {
    restoreOriginal();
    return NavigateResult::AddressSubmitFailed;
  }
  PostMessageW(controls.edit, WM_KEYUP, VK_RETURN, 0xc0000001);

  for (int i = 0; i < 25; ++i) {
    Sleep(10);
    if (!current() || !IsWindow(controls.edit)) return NavigateResult::Success;
    const auto text = readEditText(controls.edit);
    if (!text || *text != dirPath) break;
  }

  if (current() && GetForegroundWindow() == target && IsWindow(controls.edit)) {
    const auto remaining = readEditText(controls.edit);
    if (remaining && *remaining == dirPath && IsWindow(controls.okButton) &&
        IsWindowVisible(controls.okButton) && IsWindowEnabled(controls.okButton)) {
      const int okId = GetDlgCtrlID(controls.okButton);
      if (SendMessageTimeoutW(target, WM_COMMAND, MAKEWPARAM(okId, BN_CLICKED),
                              reinterpret_cast<LPARAM>(controls.okButton),
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result)) {
        for (int i = 0; i < 20; ++i) {
          Sleep(10);
          if (!current() || !IsWindow(controls.edit)) return NavigateResult::Success;
          const auto text = readEditText(controls.edit);
          if (!text || *text != dirPath) break;
        }
      }
    }
  }

  if (!originalName.empty() && current() && GetForegroundWindow() == target && IsWindow(controls.edit)) {
    if (const auto afterText = readEditText(controls.edit); afterText && afterText->empty())
      restoreOriginal();
  }
  return NavigateResult::Success;
}

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
    if (const auto classic = findClassicControls(target); classic.edit) {
      return navigateClassic(target, classic, path, thread, current, modifierPressed);
    }
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
