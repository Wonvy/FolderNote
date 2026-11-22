#include "desktop_groups_item_menu.h"
#include "language.h"
#include <commctrl.h>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT firstShellCommand = 1, lastShellCommand = 0x7fff, removeCommand = 0x8000;

struct Menu {
  HMENU handle = CreatePopupMenu();
  ~Menu() { if (handle) DestroyMenu(handle); }
};

struct FreePidl {
  using pointer = PIDLIST_ABSOLUTE;
  void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};

struct MenuMessages {
  ComPtr<IContextMenu3> menu3;
  ComPtr<IContextMenu2> menu2;

  static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto &menu = *reinterpret_cast<const MenuMessages *>(data);
    switch (message) {
    case WM_NCDESTROY:
      RemoveWindowSubclass(window, dispatch, id);
      break;
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
      if (w != 0) break;
      [[fallthrough]];
    case WM_INITMENUPOPUP:
    case WM_UNINITMENUPOPUP:
    case WM_MENUCHAR: {
      LRESULT result = 0;
      if (menu.menu3 && menu.menu3->HandleMenuMsg2(message, w, l, &result) == S_OK) return result;
      if (menu.menu2 && message != WM_MENUCHAR && menu.menu2->HandleMenuMsg(message, w, l) == S_OK) return 0;
      break;
    }
    }
    return DefSubclassProc(window, message, w, l);
  }
};
}

bool showItemMenu(HWND owner, const std::wstring &path, POINT point, bool canRemove, bool &remove, bool &rename) {
  remove = false;
  rename = false;
  if (!IsWindow(owner) || path.empty()) return false;

  PIDLIST_ABSOLUTE parsed = nullptr;
  const auto status = SHParseDisplayName(path.c_str(), nullptr, &parsed, 0, nullptr);
  const std::unique_ptr<ITEMIDLIST, FreePidl> selection(parsed);
  if (FAILED(status) || !selection) return false;

  ComPtr<IShellFolder> folder;
  ComPtr<IContextMenu> shell;
  PCUITEMID_CHILD child = nullptr;
  if (FAILED(SHBindToParent(parsed, IID_PPV_ARGS(&folder), &child)) || !child ||
      FAILED(folder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr,
                                   reinterpret_cast<void **>(shell.GetAddressOf()))) || !shell) return false;

  Menu menu;
  const bool extended = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (!menu.handle || FAILED(shell->QueryContextMenu(menu.handle, 0, firstShellCommand, lastShellCommand,
                                                   CMF_NORMAL | CMF_CANRENAME | (extended ? CMF_EXTENDEDVERBS : 0)))) return false;
  const auto count = GetMenuItemCount(menu.handle);
  if (count < 0 || (!count && !canRemove)) return false;
  if (canRemove && ((count && !AppendMenuW(menu.handle, MF_SEPARATOR, 0, nullptr)) ||
                    !AppendMenuW(menu.handle, MF_STRING, removeCommand, lang::text(L"从分组移除")))) return false;

  MenuMessages messages;
  if (FAILED(shell.As(&messages.menu3))) shell.As(&messages.menu2);
  const auto subclassId = reinterpret_cast<UINT_PTR>(&messages);
  if (!SetWindowSubclass(owner, MenuMessages::dispatch, subclassId, reinterpret_cast<DWORD_PTR>(&messages))) return false;
  SetForegroundWindow(owner);
  const auto command = TrackPopupMenuEx(menu.handle, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, owner, nullptr);
  RemoveWindowSubclass(owner, MenuMessages::dispatch, subclassId);
  PostMessageW(owner, WM_NULL, 0, 0);
  remove = command == removeCommand;
  if (command >= firstShellCommand && command <= lastShellCommand && IsWindow(owner)) {
    const UINT offset = command - firstShellCommand;
    wchar_t verbW[64]{};
    char verbA[64]{};
    const bool isRename =
        (SUCCEEDED(shell->GetCommandString(offset, GCS_VERBW, nullptr, reinterpret_cast<char *>(verbW), ARRAYSIZE(verbW))) &&
         _wcsicmp(verbW, L"rename") == 0) ||
        (SUCCEEDED(shell->GetCommandString(offset, GCS_VERBA, nullptr, verbA, ARRAYSIZE(verbA))) &&
         _stricmp(verbA, "rename") == 0);
    if (isRename) {
      rename = true;
    } else {
      CMINVOKECOMMANDINFOEX invoke{sizeof(invoke)};
      invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
      invoke.hwnd = owner;
      invoke.lpVerb = MAKEINTRESOURCEA(offset);
      invoke.lpVerbW = MAKEINTRESOURCEW(offset);
      invoke.nShow = SW_SHOWNORMAL;
      invoke.ptInvoke = point;
      shell->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO *>(&invoke));
    }
  }
  return true;
}
}
