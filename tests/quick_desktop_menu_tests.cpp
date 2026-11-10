// Exercise the actual menu command path with a hidden anchor and isolated data.
// Only popup interaction, desktop identity and launching Explorer are replaced.
#include "../src/quick_folders.h"
#include "../src/quick_popup.h"
#include "../src/navigation.h"
#include <shellapi.h>
#include <shlobj.h>
#include <iostream>
#include <stdexcept>
#include "../src/quick_popup.cpp"

namespace {
HWND anchor = nullptr;
int checks = 0, menusShown = 0, opened = 0;
bool disableOnSelection = false;
std::wstring openedPath;
void check(bool value, const char *reason) { ++checks; if (!value) throw std::runtime_error(reason); }
HWND testForeground() { return anchor; }
BOOL testExecute(SHELLEXECUTEINFOW *execute) {
  ++opened;
  PWSTR path = nullptr;
  check((execute->fMask & SEE_MASK_IDLIST) != 0 &&
        SUCCEEDED(SHGetNameFromIDList(static_cast<PCIDLIST_ABSOLUTE>(execute->lpIDList), SIGDN_FILESYSPATH, &path)),
        "desktop command opens a filesystem folder through Shell");
  openedPath = path; CoTaskMemFree(path); return TRUE;
}
}
namespace quick {
bool testDesktop(HWND target) { return target && target == anchor; }
namespace popup {
unsigned testShow(HINSTANCE, POINT, std::shared_ptr<Branch> root, ui::ThemeMode, HWND target) {
  ++menusShown; check(target == anchor, "desktop menu retains its foreground anchor");
  check(!root->items.empty() && root->items.front().actions.size() == 1 &&
        root->items.front().actions[0].enabled && root->items.front().actions[0].text.empty() &&
        (root->items.front().actions[0].symbol == Symbol::Star || root->items.front().actions[0].symbol == Symbol::Unpin),
        "no selection hides transfer buttons while retaining the enabled pin icon");
  check(root->items.front().leadingActions == 0 &&
        std::none_of(root->items.begin(), root->items.end(), [](const auto &item) {
          return item.title == L"置顶文件夹";
        }), "no-selection toolbar has only a right-aligned pin and no pinned-folders heading");
  check(root->items.front().title.empty() && root->items.back().title.empty() &&
        root->items.back().row == Row::Toolbar && root->items.back().actions.size() == 2 &&
        root->items.back().actions[0].symbol == Symbol::Settings && root->items.back().actions[1].symbol == Symbol::Trash,
        "no selection leaves footer text empty while settings and clear remain available");
  if (disableOnSelection) {
    DWORD disabled = 0;
    check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersEnabled", REG_DWORD,
                         &disabled, sizeof(disabled)) == ERROR_SUCCESS, "disable isolated setting during selection");
  }
  for (const auto &item : root->items) if (!item.iconPath.empty() && item.command) return item.command;
  throw std::runtime_error("pinned folder was not offered by desktop menu");
}
}
}
#define GetForegroundWindow testForeground
#define ShellExecuteExW testExecute
#define isDesktop testDesktop
#define show testShow
#include "../src/quick_menu.cpp"
#undef show
#undef isDesktop
#undef ShellExecuteExW
#undef GetForegroundWindow

struct Fixture {
  HKEY real = nullptr, temporary = nullptr;
  std::wstring key;
  Fixture() {
    GUID id{}; CoCreateGuid(&id); wchar_t token[40]{}; StringFromGUID2(id, token, ARRAYSIZE(token));
    key = std::wstring(L"Software\\FolderNoteDesktopMenuTests\\") + token;
    check(RegOpenCurrentUser(KEY_ALL_ACCESS, &real) == ERROR_SUCCESS &&
          RegCreateKeyExW(real, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &temporary, nullptr) == ERROR_SUCCESS &&
          RegOverridePredefKey(HKEY_CURRENT_USER, temporary) == ERROR_SUCCESS, "isolate menu settings and history");
    anchor = CreateWindowW(L"STATIC", L"Hidden desktop menu anchor", WS_OVERLAPPED, 0, 0, 100, 100,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(anchor != nullptr, "create hidden desktop anchor");
  }
  ~Fixture() {
    DestroyWindow(anchor); anchor = nullptr;
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(temporary); RegDeleteTreeW(real, key.c_str()); RegCloseKey(real);
  }
};
void transferIconTests(const std::wstring &path) {
  using namespace quick;
  const auto instance = GetModuleHandleW(nullptr);
  popup::registerPanel(instance);
  for (UINT dpi : {96u, 144u, 192u}) {
    Popup model; model.selectedCount = 3; model.canCopy = model.canMove = true; model.build(path);
    check(model.root->items.front().title.empty() &&
          model.root->items.back().title == L"已选 3 项" &&
          model.root->items.back().row == popup::Row::Toolbar && model.root->items.back().actions.size() == 2 &&
          model.root->items.back().actions[0].symbol == popup::Symbol::Settings &&
          model.root->items.back().actions[1].symbol == popup::Symbol::Trash,
          "selection count appears in the bottom settings row instead of the top action row");
    popup::Session session(instance, ui::ThemeMode::Dark);
    auto &panel = session.create(model.root, {50, 50}, false);
    session.dpi = dpi; session.populate(panel); session.place(panel, {50, 50}, false);
    session.anchor = anchor; session.armed = true; session.activated = true;
    const HWND original = session.root();
    RECT toolbar{}; SendMessageW(panel.list, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&toolbar));
    const auto copyBounds = session.actionRect(model.root->items[0], toolbar, 0);
    const auto moveBounds = session.actionRect(model.root->items[0], toolbar, 1);
    const auto pinBounds = session.actionRect(model.root->items[0], toolbar, 2);
    check(copyBounds.left < toolbar.left + (toolbar.right - toolbar.left) / 4 &&
          copyBounds.right <= moveBounds.left && moveBounds.right < pinBounds.left &&
          copyBounds.right - copyBounds.left == moveBounds.right - moveBounds.left &&
          copyBounds.right - copyBounds.left > pinBounds.right - pinBounds.left &&
          pinBounds.right > toolbar.right - (toolbar.right - toolbar.left) / 4 &&
          copyBounds.top == moveBounds.top && moveBounds.top == pinBounds.top && copyBounds.bottom == pinBounds.bottom,
          "wide copy and move buttons sit left and the pin icon sits right on the same row at each scale");
    auto clickIcon = [&](int action) {
      RECT row{}; SendMessageW(panel.list, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&row));
      auto bounds = session.actionRect(model.root->items[0], row, action);
      auto point = MAKELPARAM((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
      SendMessageW(panel.list, WM_LBUTTONDOWN, MK_LBUTTON, point);
      SendMessageW(panel.list, WM_LBUTTONUP, 0, point);
      check(!session.done && session.armed && session.activated && session.root() == original && !session.selected,
            "icon changes keep the same popup and focus alive without executing a command");
    };
    auto folderRow = [&] {
      for (size_t row = 0; row < model.root->items.size(); ++row)
        if (!model.root->items[row].iconPath.empty()) return int(row);
      throw std::runtime_error("expected a destination folder");
    };
    for (int repeat = 0; repeat < 4; ++repeat) {
      session.expand(0, folderRow()); check(session.panels.size() == 2, "open a destination submenu before switching");
      clickIcon(0);
      check(model.mode == MenuMode::Copy && session.panels.size() == 1 && model.root->items[0].actions[0].checked &&
            !model.root->items[0].actions[1].checked && model.root->items.back().title.find(L"复制 3") != std::wstring::npos,
            "copy highlights one icon, shows source count and discards stale submenus");
      clickIcon(1);
      check(model.mode == MenuMode::Move && !model.root->items[0].actions[0].checked && model.root->items[0].actions[1].checked,
            "move replaces copy mode instead of executing it");
      clickIcon(1); check(model.mode == MenuMode::Navigate, "clicking the active icon cancels transfer mode");
    }
    session.key(VK_HOME); session.key(VK_RETURN);
    check(!session.done && model.mode == MenuMode::Copy, "keyboard can select copy without closing the popup");
    session.key(VK_RIGHT); session.key(VK_RETURN);
    check(!session.done && model.mode == MenuMode::Move, "keyboard can switch to move in the same popup");
    session.activate(0, folderRow());
    check(session.done && model.commands.at(session.selected).kind == Kind::Open && model.mode == MenuMode::Move,
          "only choosing a destination commits the selected operation");
  }
  for (bool moveAllowed : {false, true}) {
    Popup model; model.canMove = moveAllowed; model.selectedCount = 1; model.build(path);
    popup::Session session(instance, ui::ThemeMode::Light); session.create(model.root, {50, 50}, false);
    session.activate(0, 0, 0);
    check(!session.done && model.mode == MenuMode::Navigate, "disabled copy cannot be invoked by mouse or keyboard");
    session.key(VK_HOME);
    check(session.panels[0]->selected == 0 && session.panels[0]->selectedAction == (moveAllowed ? 1 : 2),
          "keyboard skips disabled icons");
    session.key(VK_RIGHT);
    if (moveAllowed) {
      check(session.panels[0]->selectedAction == 2, "right moves from the left transfer group to the right pin icon");
      session.key(VK_RIGHT);
      check(session.panels[0]->selectedAction == 1, "keyboard wraps to move while skipping disabled copy");
      session.key(VK_RETURN); check(!session.done && model.mode == MenuMode::Move, "available move works independently of copy");
    }
  }
  for (DWORD selectedCount : {0u, 3u}) {
    Popup model; model.showCurrentName = true; model.selectedCount = selectedCount;
    model.canCopy = model.canMove = selectedCount != 0; model.build(path);
    const auto currentName = std::filesystem::path(path).filename().native();
    check(model.root->items.front().title == (selectedCount ? L"" : currentName) &&
          model.root->items.front().actions.size() == (selectedCount ? 3u : 1u) &&
          model.root->items.front().leadingActions == (selectedCount ? 2u : 0u) &&
          model.root->items.back().title == (selectedCount ? L"已选 3 项" : L""),
          "blank-area menu shows either leading transfer buttons or the current name, with count only in the footer");
    if (selectedCount) {
      for (int action : {0, 1}) {
        auto invoke = model.root->invoke;
        check(invoke(model.root->items.front().actions[action].command) &&
              model.root->items.front().title.empty() &&
              model.root->items.back().title.find(action == 0 ? L"复制 3" : L"移动 3") != std::wstring::npos,
              "mode switching keeps the folder caption hidden while selected-item status updates in the footer");
      }
    }
  }
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try {
    Fixture fixture;
    DWORD enabled = 1, dialogs = 0;
    check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersEnabled", REG_DWORD, &enabled, sizeof(enabled)) == ERROR_SUCCESS &&
          RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\FolderNote", L"QuickFoldersInDialogs", REG_DWORD, &dialogs, sizeof(dialogs)) == ERROR_SUCCESS,
          "enable desktop menu without file dialog support");
    wchar_t temp[MAX_PATH]{}; check(GetTempPathW(ARRAYSIZE(temp), temp) != 0, "get existing local folder");
    const auto path = quick::normalize(temp); quick::pin(path, true);
    transferIconTests(path);
    for (auto mode : {quick::MenuMode::Copy, quick::MenuMode::Move}) {
      quick::Popup popup; popup.mode = mode; popup.selectedCount = 3; popup.canCopy = popup.canMove = true; popup.build(path);
      check(popup.root->items.front().title.empty() &&
            popup.root->items.back().title.find(mode == quick::MenuMode::Copy ? L"复制 3" : L"移动 3") != std::wstring::npos,
            "transfer popup identifies action and selection count in the bottom settings row");
      check(popup.root->items.front().actions.size() == 3 && popup.root->items.front().leadingActions == 2 &&
            popup.root->items.front().actions[0].text == L"复制" && popup.root->items.front().actions[1].text == L"移动" &&
            popup.root->items.front().actions[2].text.empty() &&
            popup.commands.at(popup.root->items.front().actions[2].command).kind == quick::Kind::Unpin,
            "transfer toolbar retains copy and move text buttons plus the current-folder pin icon");
      for (const auto &[id, command] : popup.commands) {
        (void)id;
        check(command.kind != quick::Kind::NewWindow, "transfer folder commands never launch a navigation window");
      }
    }
    check(quick::menu(GetModuleHandleW(nullptr), anchor, nullptr, {20, 20}) == 0, "desktop menu command succeeds");
    check(menusShown == 1 && opened == 1 && quick::same(openedPath, path), "ordinary desktop folder selection opens one Explorer window");
    check(!quick::load().history.empty() && quick::same(quick::load().history.front(), path), "successful desktop folder open is recorded");
    disableOnSelection = true;
    check(quick::menu(GetModuleHandleW(nullptr), anchor, nullptr, {20, 20}) == 0 && menusShown == 2 && opened == 1,
          "disabling the feature before command dispatch prevents opening Explorer");
    check(quick::menu(GetModuleHandleW(nullptr), anchor, nullptr, {20, 20}) == 1 && menusShown == 2,
          "disabled desktop feature never displays the menu");
    std::cout << "PASS " << checks << " desktop menu assertions: folder opening, current folder, history, cancellation\n";
  } catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; result = 1; }
  CoUninitialize(); return result;
}
