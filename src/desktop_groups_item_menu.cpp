#include "desktop_groups_item_menu.h"
#include "language.h"
#include <commctrl.h>
#include <memory>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>
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

ItemMenuResult showItemMenu(HWND owner, std::span<const std::wstring> paths, POINT point, bool canRemove) {
  ItemMenuResult outcome;
  if (!IsWindow(owner) || paths.empty() || paths.front().empty()) return outcome;

  using OwnedPidl = std::unique_ptr<ITEMIDLIST, FreePidl>;
  std::vector<OwnedPidl> parsedList;
  parsedList.reserve(paths.size());
  for (const auto &p : paths) {
    if (p.empty()) continue;
    PIDLIST_ABSOLUTE id = nullptr;
    if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr)) && id) {
      parsedList.emplace_back(id);
    }
  }
  if (parsedList.empty()) return outcome;

  ComPtr<IShellFolder> folder;
  ComPtr<IContextMenu> shell;
  if (parsedList.size() > 1) {
    OwnedPidl firstParent(ILCloneFull(parsedList.front().get()));
    if (firstParent) {
      ILRemoveLastID(firstParent.get());
      bool sameParent = true;
      std::vector<PCUITEMID_CHILD> children;
      children.reserve(parsedList.size());
      for (const auto &item : parsedList) {
        OwnedPidl candidateParent(ILCloneFull(item.get()));
        if (!candidateParent) { sameParent = false; break; }
        ILRemoveLastID(candidateParent.get());
        if (!ILIsEqual(firstParent.get(), candidateParent.get())) { sameParent = false; break; }
        children.push_back(ILFindLastID(item.get()));
      }
      if (sameParent && !children.empty()) {
        PCUITEMID_CHILD unusedChild = nullptr;
        if (SUCCEEDED(SHBindToParent(parsedList.front().get(), IID_PPV_ARGS(&folder), &unusedChild)) && folder) {
          folder->GetUIObjectOf(owner, static_cast<UINT>(children.size()), children.data(), IID_IContextMenu,
                                nullptr, reinterpret_cast<void **>(shell.GetAddressOf()));
        }
      }
    }
  }
  if (!shell) {
    folder.Reset();
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(parsedList.front().get(), IID_PPV_ARGS(&folder), &child)) || !folder || !child ||
        FAILED(folder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr,
                                     reinterpret_cast<void **>(shell.GetAddressOf()))) || !shell)
      return outcome;
  }

  Menu menu;
  const bool extended = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const UINT queryFlags = CMF_NORMAL | (parsedList.size() == 1 ? CMF_CANRENAME : 0) | (extended ? CMF_EXTENDEDVERBS : 0);
  if (!menu.handle || FAILED(shell->QueryContextMenu(menu.handle, 0, firstShellCommand, lastShellCommand, queryFlags)))
    return outcome;
  const auto count = GetMenuItemCount(menu.handle);
  if (count < 0 || (!count && !canRemove)) return outcome;
  if (canRemove && ((count && !AppendMenuW(menu.handle, MF_SEPARATOR, 0, nullptr)) ||
                    !AppendMenuW(menu.handle, MF_STRING, removeCommand, lang::text(L"从分组移除")))) return outcome;

  MenuMessages messages;
  if (FAILED(shell.As(&messages.menu3))) shell.As(&messages.menu2);
  const auto subclassId = reinterpret_cast<UINT_PTR>(&messages);
  if (!SetWindowSubclass(owner, MenuMessages::dispatch, subclassId, reinterpret_cast<DWORD_PTR>(&messages))) return outcome;
  SetForegroundWindow(owner);
  const auto command = TrackPopupMenuEx(menu.handle, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, owner, nullptr);
  RemoveWindowSubclass(owner, MenuMessages::dispatch, subclassId);
  PostMessageW(owner, WM_NULL, 0, 0);
  outcome.shown = true;
  outcome.remove = command == removeCommand;
  if (command >= firstShellCommand && command <= lastShellCommand && IsWindow(owner)) {
    const UINT offset = command - firstShellCommand;
    wchar_t verbW[64]{};
    char verbA[64]{};
    const bool gotW = SUCCEEDED(shell->GetCommandString(offset, GCS_VERBW, nullptr,
                                                        reinterpret_cast<char *>(verbW), ARRAYSIZE(verbW)));
    const bool gotA = SUCCEEDED(shell->GetCommandString(offset, GCS_VERBA, nullptr, verbA, ARRAYSIZE(verbA)));
    const auto matchesVerb = [&](const wchar_t *wide, const char *narrow) {
      return (gotW && _wcsicmp(verbW, wide) == 0) || (gotA && _stricmp(verbA, narrow) == 0);
    };
    if (matchesVerb(L"rename", "rename")) {
      outcome.rename = true;
    } else if (matchesVerb(L"delete", "delete")) {
      outcome.deleteItem = true;
      outcome.permanentDelete = extended || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    } else {
      CMINVOKECOMMANDINFOEX invoke{sizeof(invoke)};
      invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
      invoke.hwnd = owner;
      invoke.lpVerb = MAKEINTRESOURCEA(offset);
      invoke.lpVerbW = MAKEINTRESOURCEW(offset);
      invoke.nShow = SW_SHOWNORMAL;
      invoke.ptInvoke = point;
      if (SUCCEEDED(shell->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO *>(&invoke))))
        outcome.invokedShellCommand = true;
    }
  }
  return outcome;
}
}
