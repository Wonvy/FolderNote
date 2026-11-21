#include "quick_find_controls.h"
#include "quick_find.h"
#include "quick_find_popup.h"
#include <commctrl.h>
#include <memory>

namespace quick_find {
bool shortcutModifiers() {
  return ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) | GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
}
namespace {
constexpr UINT_PTR subclassId = 0x464E5046;
struct Control { Popup popup; bool listView; std::function<void(int)> openFolder; std::function<CandidateIcon(int)> icon; };
std::vector<std::wstring> names(HWND window, bool listView) {
  const auto count = listView ? ListView_GetItemCount(window) : static_cast<int>(SendMessageW(window, LB_GETCOUNT, 0, 0));
  std::vector<std::wstring> result;
  for (int i = 0; i < count; ++i) {
    std::wstring text;
    if (listView) {
      text.resize(32768);
      LVITEMW item{}; item.pszText = text.data(); item.cchTextMax = static_cast<int>(text.size());
      const auto length = SendMessageW(window, LVM_GETITEMTEXTW, i, reinterpret_cast<LPARAM>(&item));
      text.resize(static_cast<size_t>(length));
    } else {
      const auto length = SendMessageW(window, LB_GETTEXTLEN, i, 0);
      if (length >= 0 && length < 32768) {
        text.resize(static_cast<size_t>(length) + 1);
        SendMessageW(window, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(length));
      }
    }
    result.emplace_back(text);
  }
  return result;
}
void select(HWND window, bool listView, int row) {
  if (row < 0) return;
  if (listView) {
    ListView_SetItemState(window, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(window, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(window, row, FALSE);
  } else {
    SendMessageW(window, LB_SETCURSEL, row, 0);
    // Programmatic ListBox selection does not emit LBN_SELCHANGE by itself.
    SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), LBN_SELCHANGE), reinterpret_cast<LPARAM>(window));
  }
}
LRESULT CALLBACK input(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto state = reinterpret_cast<Control *>(data);
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, input, id);
    delete state;
    return DefSubclassProc(window, message, w, l);
  }
  try {
    if (message == WM_GETDLGCODE && l && state->popup.active()) {
      const auto key = reinterpret_cast<const MSG *>(l);
      if (key->message == WM_KEYDOWN && (key->wParam == VK_ESCAPE || key->wParam == VK_BACK || key->wParam == VK_RETURN ||
          (key->wParam >= VK_PRIOR && key->wParam <= VK_DOWN)))
        return DefSubclassProc(window, message, w, l) | DLGC_WANTMESSAGE;
    }
    if (message == WM_KILLFOCUS || message == WM_LBUTTONDOWN || message == LB_RESETCONTENT ||
        message == LVM_DELETEALLITEMS) state->popup.close();
    if (GetFocus() == window && !shortcutModifiers()) {
      if (message == WM_CHAR && w == L'\b') return 0; // Handled once at keydown.
      if (message == WM_CHAR && inputCharacter(static_cast<wchar_t>(w))) {
        if (!state->popup.active()) {
          const auto items = names(window, state->listView);
          std::vector<CandidateIcon> icons;
          for (size_t row = 0; row < items.size(); ++row) {
            if (state->icon) { icons.push_back(state->icon(static_cast<int>(row))); continue; }
            CandidateIcon icon;
            if (state->listView) {
              const auto images = ListView_GetImageList(window, LVSIL_SMALL);
              LVITEMW item{}; item.mask = LVIF_IMAGE | LVIF_STATE; item.iItem = static_cast<int>(row); item.stateMask = LVIS_OVERLAYMASK;
              if (images && ListView_GetItem(window, &item) && item.iImage >= 0) {
                if (auto image = ImageList_GetIcon(images, item.iImage, ILD_NORMAL | (item.state & LVIS_OVERLAYMASK))) {
                  icon = CandidateIcon::copy(image); DestroyIcon(image);
                }
              }
            } else if (items[row].starts_with(L"\\\\") || (items[row].size() > 2 && items[row][1] == L':')) {
              icon.source = quick::icons::Source::file(items[row]);
            }
            icons.push_back(std::move(icon));
          }
          state->popup.begin(window, items, [window, listView = state->listView, items, openFolder = state->openFolder](int row) {
            const auto current = names(window, listView);
            if (row >= 0 && size_t(row) < current.size() && current[row] == items[row]) {
              select(window, listView, row);
              if (openFolder && IsWindow(window)) openFolder(row);
            }
          }, std::move(icons));
        }
        state->popup.character(static_cast<wchar_t>(w));
        return 0;
      }
      if (message == WM_KEYDOWN && state->popup.key(static_cast<UINT>(w))) return 0;
    }
    if (message == WM_KEYDOWN && shortcutModifiers()) state->popup.close();
  } catch (...) { state->popup.close(); }
  return DefSubclassProc(window, message, w, l);
}
}
void attach(HWND list, std::function<void(int)> openFolder, std::function<CandidateIcon(int)> icon) {
  if (!list || GetWindowThreadProcessId(list, nullptr) != GetCurrentThreadId()) return;
  DWORD_PTR existing = 0;
  if (GetWindowSubclass(list, input, subclassId, &existing)) return;
  wchar_t type[64]{};
  GetClassNameW(list, type, ARRAYSIZE(type));
  const bool listView = wcscmp(type, WC_LISTVIEWW) == 0;
  if (!listView && (wcscmp(type, L"ListBox") != 0 ||
      ((GetWindowLongPtrW(list, GWL_STYLE) & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) &&
       !(GetWindowLongPtrW(list, GWL_STYLE) & LBS_HASSTRINGS)))) return;
  auto state = std::make_unique<Control>();
  state->listView = listView;
  state->openFolder = std::move(openFolder);
  state->icon = std::move(icon);
  if (SetWindowSubclass(list, input, subclassId, reinterpret_cast<DWORD_PTR>(state.get()))) state.release();
}
}
