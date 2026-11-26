#include "quick_folders.h"
#include "language.h"
#include "quick_popup.h"
#include "quick_menu_gate.h"
#include "quick_dialog.h"
#include "quick_dialog_history.h"
#include "quick_legacy.h"
#include "quick_transfer.h"
#include "quick_hit.h"
#include "navigation.h"
#include "core.h"
#include "window_position.h"
#include "runtime_guard.h"
#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
using Microsoft::WRL::ComPtr;
namespace quick {
namespace {
ComPtr<IShellBrowser> browserFor(HWND target, HWND expectedView, HWND *actualView = nullptr) {
  ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return {};
  if (isDesktop(target)) {
    VARIANT location{}; location.vt = VT_I4; location.lVal = CSIDL_DESKTOP;
    VARIANT root{}; long handle = 0;
    ComPtr<IDispatch> dispatch; ComPtr<IServiceProvider> services; ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view; HWND viewWindow = nullptr;
    if (FAILED(windows->FindWindowSW(&location, &root, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, &dispatch)) || !dispatch ||
        FAILED(dispatch.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser ||
        FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view->GetWindow(&viewWindow)) ||
        !IsWindowVisible(viewWindow) || (expectedView && viewWindow != expectedView) ||
        !isDesktop(GetAncestor(viewWindow, GA_ROOT))) return {};
    if (actualView) *actualView = viewWindow;
    return browser;
  }
  long count = 0; windows->get_Count(&count);
  for (long i = 0; i < count; ++i) {
    VARIANT index{}; index.vt = VT_I4; index.lVal = i;
    ComPtr<IDispatch> dispatch;
    if (FAILED(windows->Item(index, &dispatch)) || !dispatch) continue;
    ComPtr<IWebBrowser2> web;
    SHANDLE_PTR handle = 0;
    if (FAILED(dispatch.As(&web)) || FAILED(web->get_HWND(&handle)) ||
        GetAncestor(reinterpret_cast<HWND>(handle), GA_ROOT) != target) continue;
    ComPtr<IServiceProvider> services; ComPtr<IShellBrowser> browser;
    if (FAILED(dispatch.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || !browser) continue;
    ComPtr<IShellView> view; HWND viewWindow = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view->GetWindow(&viewWindow)) ||
        !IsWindowVisible(viewWindow) || (expectedView && expectedView != viewWindow) ||
        GetAncestor(viewWindow, GA_ROOT) != target) continue;
    if (actualView) *actualView = viewWindow;
    return browser;
  }
  return {};
}
struct Scan {
  std::mutex mutex; std::condition_variable ready;
  bool done = false, more = false; DWORD error = 0;
  std::vector<std::wstring> folders;
};
std::shared_ptr<Scan> scan(const std::wstring &path) {
  auto result = std::make_shared<Scan>();
  std::thread([result, path] {
    std::vector<std::wstring> folders; DWORD error = 0; bool more = false;
    try {
      auto extended = path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
      auto wildcard = extended + (extended.back() == L'\\' ? L"*" : L"\\*");
      WIN32_FIND_DATAW entry{};
      HANDLE find = FindFirstFileExW(wildcard.c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
      if (find == INVALID_HANDLE_VALUE) error = GetLastError();
      else {
        unsigned checked = 0;
        do {
          if (++checked > 5000 || folders.size() >= 150) { more = true; break; }
          if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
              (entry.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ||
              !wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
          folders.push_back(path + (path.back() == L'\\' ? L"" : L"\\") + entry.cFileName);
        } while (FindNextFileW(find, &entry));
        FindClose(find);
      }
      std::sort(folders.begin(), folders.end(), [](const auto &a, const auto &b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
      });
    } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    { std::lock_guard lock(result->mutex); result->folders = std::move(folders); result->error = error; result->more = more; result->done = true; }
    result->ready.notify_all();
  }).detach();
  return result;
}
bool directoryAvailable(const std::wstring &path) {
  auto state = std::make_shared<Scan>();
  std::thread([state, path] {
    DWORD attributes = GetFileAttributesW(path.c_str());
    { std::lock_guard lock(state->mutex); state->done = true;
      state->error = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : ERROR_PATH_NOT_FOUND; }
    state->ready.notify_all();
  }).detach();
  std::unique_lock lock(state->mutex);
  return state->ready.wait_for(lock, std::chrono::milliseconds(1500), [&] { return state->done; }) && !state->error;
}
std::wstring label(const std::wstring &path) {
  auto title = std::filesystem::path(path).filename().native();
  return title.empty() ? path : title;
}
enum class Kind { Open, NewWindow, Pin, Unpin, Manage, Clear, Copy, Move, Recent, Frequent };
struct Command { Kind kind; std::wstring path; };
struct Popup {
  using Branch = popup::Branch;
  using Item = popup::Item;
  using Symbol = popup::Symbol;
  using Row = popup::Row;
  std::shared_ptr<Branch> root = std::make_shared<Branch>();
  std::map<UINT, Command> commands;
  UINT next = 1;
  State state;
  MenuMode mode = MenuMode::Navigate;
  DWORD selectedCount = 0;
  bool canCopy = false, canMove = false;
  bool showCurrentName = false;
  bool loaded = false;
  bool showFrequent = false;
  struct Known { std::wstring path, title; Symbol symbol; };
  std::vector<Known> known;
  bool pinned(const std::wstring &path) { return std::any_of(state.pins.begin(), state.pins.end(), [&](const auto &p) { return same(p, path); }); }
  void heading(Branch &branch, const std::wstring &text) { branch.items.push_back({text, 0, Symbol::Star, {}, Row::Heading}); }
  void hint(Branch &branch, const std::wstring &text) { branch.items.push_back({text, 0, Symbol::Folder, {}, Row::Hint}); }
  void separator(Branch &branch) { branch.items.push_back({L"", 0, Symbol::Folder, {}, Row::Separator}); }
  UINT command(Kind kind, const std::wstring &path = {}) {
    UINT id = next++; commands.emplace(id, Command{kind, path}); return id;
  }
  void add(Branch &branch, const std::wstring &text, Kind kind, Symbol icon, const std::wstring &path = {}) {
    branch.items.push_back({text, command(kind, path), icon});
  }
  void folder(Branch &branch, const std::wstring &path, unsigned depth = 0) {
    auto children = std::make_shared<Branch>();
    add(*children, mode == MenuMode::Copy ? lang::text(L"复制到此文件夹") : mode == MenuMode::Move ? lang::text(L"移动到此文件夹") : lang::text(L"在新窗口打开"),
        mode == MenuMode::Navigate ? Kind::NewWindow : Kind::Open,
        mode == MenuMode::Copy ? Symbol::Copy : mode == MenuMode::Move ? Symbol::Move : Symbol::Window, path);
    add(*children, pinned(path) ? lang::text(L"取消置顶") : lang::text(L"置顶此文件夹"), pinned(path) ? Kind::Unpin : Kind::Pin, pinned(path) ? Symbol::Unpin : Symbol::Star, path);
    separator(*children);
    if (depth >= 8) hint(*children, lang::text(L"更深的目录请直接打开"));
    else {
      hint(*children, lang::text(L"正在读取子文件夹…"));
      children->poll = [this, path, depth, listing = std::shared_ptr<Scan>{}](Branch &target) mutable {
        if (!listing) { listing = scan(path); return false; }
        std::unique_lock lock(listing->mutex);
        if (!listing->done) return false;
        target.items.resize(3);
        if (listing->error && listing->error != ERROR_FILE_NOT_FOUND) hint(target, lang::text(L"目录暂不可用或无权访问"));
        else if (listing->folders.empty()) hint(target, lang::text(L"没有子文件夹"));
        else for (const auto &child : listing->folders) folder(target, child, depth + 1);
        if (listing->more) hint(target, lang::text(L"更多内容请打开此文件夹"));
        return true;
      };
    }
    auto title = label(path); auto symbol = Symbol::Folder;
    for (const auto &value : known) if (same(value.path, path)) { title = value.title; symbol = value.symbol; break; }
    branch.items.push_back({title, command(Kind::Open, path), symbol, children, Row::Action, path});
  }
  void build(const std::wstring &current) {
    root->items.clear(); commands.clear(); next = 1;
    // Mode changes only rebuild the model; do not repeat Shell/registry queries
    // inside the live popup's mouse/keyboard dispatch.
    if (!loaded) {
      state = load();
      const KNOWNFOLDERID *ids[]{&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads};
      const wchar_t *names[]{lang::text(L"桌面"), lang::text(L"文档"), lang::text(L"下载")};
      const Symbol symbols[]{Symbol::Desktop, Symbol::Documents, Symbol::Downloads};
      for (size_t i = 0; i < 3; ++i) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*ids[i], KF_FLAG_DONT_VERIFY, nullptr, &path))) { known.push_back({path, names[i], symbols[i]}); CoTaskMemFree(path); }
      }
      loaded = true;
    }
    heading(*root, L"");
    if (showCurrentName && !selectedCount && !current.empty()) {
      root->items.back().title = label(current);
      for (const auto &place : known) if (same(place.path, current)) { root->items.back().title = place.title; break; }
    }
    auto actionTitle = [&](MenuMode action, bool enabled) {
      const bool copying = action == MenuMode::Copy;
      if (!enabled) return copying ? lang::text(L"复制：所选项目不支持此操作") : lang::text(L"移动：所选项目不支持此操作");
      if (mode == action) return copying ? lang::text(L"取消复制，返回快速定位") : lang::text(L"取消移动，返回快速定位");
      return copying ? lang::text(L"复制所选项目到…") : lang::text(L"移动所选项目到…");
    };
    if (selectedCount) {
      root->items.back().actions = {
          {command(Kind::Copy), Symbol::Copy, actionTitle(MenuMode::Copy, canCopy), canCopy, mode == MenuMode::Copy, lang::text(L"复制")},
          {command(Kind::Move), Symbol::Move, actionTitle(MenuMode::Move, canMove), canMove, mode == MenuMode::Move, lang::text(L"移动")}};
      root->items.back().leadingActions = 2;
    }
    if (!current.empty()) root->items.back().actions.push_back({command(pinned(current) ? Kind::Unpin : Kind::Pin, current), pinned(current) ? Symbol::Unpin : Symbol::Star,
        pinned(current) ? lang::text(L"取消置顶当前文件夹") : lang::text(L"置顶当前文件夹")});
    if (root->items.back().actions.empty()) root->items.clear();
    else {
      if (mode != MenuMode::Navigate) hint(*root, lang::text(L"单击目标文件夹，悬停展开子目录"));
      separator(*root);
    }
    if (state.pins.empty()) hint(*root, lang::text(L"暂无置顶"));
    for (const auto &path : state.pins) folder(*root, path);
    separator(*root);
    Item tabs; tabs.row = Row::Tabs;
    tabs.actions = {
        {command(Kind::Recent), Symbol::History, lang::text(L"最近访问"), true, !showFrequent, lang::text(L"最近访问")},
        {command(Kind::Frequent), Symbol::History, lang::text(L"最多访问"), true, showFrequent, lang::text(L"最多访问")}};
    root->items.push_back(std::move(tabs));
    const size_t limit = showFrequent ? 10 : 8;
    std::vector<std::wstring> shown;
    auto addLocations = [&](const auto &paths) {
      for (const auto &path : paths) {
        if (shown.size() == limit) break;
        if (pinned(path) || std::any_of(shown.begin(), shown.end(), [&](const auto &p) { return same(p, path); })) continue;
        shown.push_back(path);
      }
    };
    if (showFrequent) addLocations(state.frequent);
    addLocations(state.history);
    for (const auto &path : shown) {
      folder(*root, path);
    }
    if (shown.empty()) hint(*root, lang::text(L"暂无记录"));
    auto history = std::make_shared<Branch>();
    heading(*history, lang::text(L"历史记录 · 最近 20 条"));
    for (const auto &path : state.history) folder(*history, path);
    if (state.history.empty()) hint(*history, lang::text(L"暂无历史记录"));
    root->items.push_back({lang::text(L"历史记录（最近 20 条）"), 0, Symbol::More, history});
    if (mode != MenuMode::Navigate) {
      separator(*root); heading(*root, lang::text(L"常用位置"));
      for (const auto &place : known) folder(*root, place.path);
    }
    separator(*root);
    Item toolbar; toolbar.row = Row::Toolbar;
    if (selectedCount) {
      const auto count = std::to_wstring(selectedCount);
      if (mode == MenuMode::Navigate) toolbar.title = lang::format(L"已选 {0} 项", {count});
      else if (mode == MenuMode::Copy) toolbar.title = lang::format(L"复制 {0} 项到…", {count});
      else toolbar.title = lang::format(L"移动 {0} 项到…", {count});
    }
    toolbar.actions = {{command(Kind::Manage), Symbol::Settings, lang::text(L"管理置顶与历史")}, {command(Kind::Clear), Symbol::Trash, lang::text(L"清空历史")}};
    root->items.push_back(std::move(toolbar));
    root->invoke = [this, current](unsigned selected) {
      const auto found = commands.find(selected);
      if (found == commands.end()) return false;
      const auto kind = found->second.kind;
      if (kind == Kind::Recent || kind == Kind::Frequent) {
        showFrequent = kind == Kind::Frequent;
        build(current); return true;
      }
      if (kind != Kind::Copy && kind != Kind::Move) return false;
      if ((kind == Kind::Copy && !canCopy) || (kind == Kind::Move && !canMove)) return true;
      const auto requested = kind == Kind::Copy ? MenuMode::Copy : MenuMode::Move;
      mode = mode == requested ? MenuMode::Navigate : requested;
      build(current); return true;
    };
  }
};
}
int menu(HINSTANCE instance, HWND target, HWND view, POINT point) {
  const bool desktop = isDesktop(target);
  if (!navigation::quickEnabled() || (!desktop && !isExplorer(target) && !isFileDialog(target))) return 1;
  const bool inDesktopBox = [&] {
    if (!desktop) return false;
    const HWND under = WindowFromPoint(point), underRoot = under ? GetAncestor(under, GA_ROOT) : nullptr;
    if (!under || !isDesktop(underRoot)) return false;
    for (HWND h = under; h && h != underRoot; h = GetParent(h)) {
      wchar_t cls[128]{};
      GetClassNameW(h, cls, ARRAYSIZE(cls));
      if (!wcscmp(cls, L"FolderNote.DesktopGroups.Panel")) return true;
    }
    return false;
  }();
  const auto trayWindow = [](HWND h) {
    wchar_t cls[64]{};
    return h && GetClassNameW(h, cls, ARRAYSIZE(cls)) &&
           (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"SecondaryTrayWnd"));
  };
  const HWND initialForeground = GetForegroundWindow();
  const auto foregroundOk = [&] {
    const HWND currentForeground = GetForegroundWindow();
    return currentForeground == target ||
           (desktop && (isDesktop(currentForeground) || trayWindow(currentForeground))) ||
           (inDesktopBox && currentForeground == initialForeground);
  };
  if (!foregroundOk()) return 0;
  DWORD targetPid = 0; GetWindowThreadProcessId(target, &targetPid);
  auto requestName = std::wstring(L"Local\\FolderNote.QuickFolders.Request.v1.") + std::to_wstring(targetPid) + L"." +
      std::to_wstring(reinterpret_cast<UINT_PTR>(target));
  detail::MenuRequest request(requestName.c_str());
  bool started = false;
  int result = 0;
  auto validTarget = [&] {
    DWORD currentPid = 0;
    return IsWindow(target) && GetWindowThreadProcessId(target, &currentPid) && currentPid == targetPid;
  };
  auto currentRequest = [&] { return request.current() && navigation::quickEnabled() && validTarget(); };
  try {
    if (!request.valid()) fn::fail(lang::text(L"无法准备快速文件夹菜单"), request.error());
    std::wstring current;
    ComPtr<IShellItemArray> selection;
    DWORD selectedCount = 0;
    bool showCurrentName = false;
    bool showFrequent = false;
    MenuMode mode = MenuMode::Navigate;
    bool repeat = true;
    while (repeat && navigation::quickEnabled()) {
      repeat = false;
      Popup popup;
      UINT selected = 0;
      {
        detail::MenuGate gate;
        if (!gate.acquire()) {
          if (gate.error() != ERROR_TIMEOUT) fn::fail(lang::text(L"无法打开快速文件夹菜单"), gate.error());
          guard::log(L"中键菜单被跳过：上一个菜单仍占用菜单锁");
          break;
        }
        if (!validTarget() || !foregroundOk() || !navigation::quickEnabled()) break;
        if (!started) {
          request.begin(); started = true;
          // Keep preparation serialized too: a slow Shell provider must not
          // accumulate one blocked query process per repeated middle click.
          if (desktop || isExplorer(target)) {
            auto browser = browserFor(target, view, &view);
            ComPtr<IShellView> active;
            if (browser && SUCCEEDED(browser->QueryActiveShellView(&active)) && active) {
              if (!desktop) current = viewPath(active.Get());
              // Read the active view's selection regardless of the click position,
              // then freeze it before the user chooses an operation/destination.
              selection = transfer::selectedItems(active.Get(), selectedCount);
              if (!browserFor(target, view) || !foregroundOk()) { selection.Reset(); selectedCount = 0; }
            }
          } else current = dialog::currentPath(target);
          if (desktop) {
            PWSTR path = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DONT_VERIFY, nullptr, &path))) current = path;
            CoTaskMemFree(path);
          }
          if (!current.empty() && !selectedCount) {
            HWND hitView = view;
            if (!hitView) {
              // File dialogs have no IShellBrowser; locate their file-list
              // surface under the click, without treating a tree as a list.
              for (HWND child = WindowFromPoint(point); child && child != target && IsChild(target, child); child = GetParent(child)) {
                wchar_t cls[128]{}; GetClassNameW(child, cls, ARRAYSIZE(cls));
                if (!wcscmp(cls, L"SHELLDLL_DefView") || !wcscmp(cls, L"SysListView32")) { hitView = child; break; }
              }
            }
            showCurrentName = detail::blankSurface(hitView, point);
          }
          if (!current.empty()) touchHistory(current);
        }
        if (!currentRequest() || !foregroundOk()) break;
        popup.mode = mode; popup.selectedCount = selectedCount;
        popup.showCurrentName = showCurrentName;
        popup.showFrequent = showFrequent;
        popup.canCopy = transfer::supports(selection.Get(), MenuMode::Copy);
        popup.canMove = transfer::supports(selection.Get(), MenuMode::Move);
        popup.build(current);
        // Slow queries must not reopen after focus changes or a newer request.
        if (!currentRequest() || !foregroundOk()) break;
        selected = popup::show(instance, point, popup.root, ui::configuredTheme(), target);
        mode = popup.mode;
        showFrequent = popup.showFrequent;
      } // The hidden popup no longer owns the gate, even if its command is slow.
      if (!selected || !popup.commands.contains(selected) || !currentRequest()) break;
      auto command = popup.commands.at(selected);
      if (command.kind == Kind::Pin || command.kind == Kind::Unpin) {
        pin(command.path, command.kind == Kind::Pin); repeat = true;
      } else if (command.kind == Kind::Manage) manage(instance);
      else if (command.kind == Kind::Clear) {
        const auto answer = ui::message(nullptr, lang::text(L"清空访问历史和访问次数？置顶文件夹将保留。"), lang::text(L"快速文件夹"), MB_YESNO | MB_ICONQUESTION);
        if (!currentRequest()) break;
        if (answer == IDYES) clearHistory();
        repeat = true;
      } else if (command.kind == Kind::Open || command.kind == Kind::NewWindow) {
        const bool available = directoryAvailable(command.path);
        if (!currentRequest()) break;
        if (!available) throw fn::Error{lang::text(L"该文件夹不存在、无权访问或响应超时。置顶记录已保留。")};
        PIDLIST_ABSOLUTE parsed = nullptr;
        const auto parsedResult = SHParseDisplayName(command.path.c_str(), nullptr, &parsed, 0, nullptr);
        struct ParsedId {
          PIDLIST_ABSOLUTE value;
          ~ParsedId() { CoTaskMemFree(value); }
          PIDLIST_ABSOLUTE get() const noexcept { return value; }
        } id{parsed};
        if (!currentRequest()) break;
        if (FAILED(parsedResult)) throw fn::Error{lang::text(L"无法打开这个文件夹。")};
        if (mode != MenuMode::Navigate) {
          if (!selection || !browserFor(target, view) || !transfer::supports(selection.Get(), mode)) break;
          ComPtr<IShellItem> destination;
          const auto created = SHCreateItemFromIDList(id.get(), IID_PPV_ARGS(&destination));
          if (FAILED(created)) fn::fail(lang::text(L"无法访问目标文件夹"), static_cast<DWORD>(created));
          if (!currentRequest()) break;
          const auto transferred = transfer::perform(target, selection.Get(), destination.Get(), mode, currentRequest);
          if (FAILED(transferred)) fn::fail(lang::text(L"无法完成复制 / 移动操作"), static_cast<DWORD>(transferred));
          if (transferred == S_OK) visit(command.path);
          break;
        }
        bool ok = false;
        if (desktop || command.kind == Kind::NewWindow) {
          SHELLEXECUTEINFOW execute{sizeof(execute)}; execute.fMask = SEE_MASK_IDLIST; execute.lpIDList = id.get(); execute.nShow = SW_SHOWNORMAL;
          ok = ShellExecuteExW(&execute) != FALSE;
        } else if (isExplorer(target)) {
          auto browser = browserFor(target, view);
          if (!currentRequest()) break;
          if (browser) {
            SetForegroundWindow(target);
            if (!currentRequest()) break;
            ok = SUCCEEDED(browser->BrowseObject(id.get(), SBSP_ABSOLUTE | SBSP_SAMEBROWSER));
          }
        } else if (legacy::isDialog(target)) ok = legacy::navigate(target, command.path, request);
        else {
          const auto navigated = dialog::navigate(target, command.path, request);
          if (!currentRequest()) break;
          if (navigated != dialog::NavigateResult::Success) throw fn::Error{dialog::failureMessage(navigated)};
          ok = true;
        }
        if (!currentRequest()) break;
        if (!ok) throw fn::Error{lang::text(L"未能在原窗口定位文件夹。请确认窗口或标签页未切换，且程序未以管理员权限运行。\n不支持的自定义文件对话框可使用“在新窗口打开”。")};
        visit(command.path);
      }
    }
  } catch (const fn::Error &error) {
    // A late failure from an obsolete request must not steal focus back.
    if ((!started || request.current()) && navigation::quickEnabled() && validTarget())
      ui::message(nullptr, error.message.c_str(), lang::text(L"快速文件夹"), MB_OK | MB_ICONINFORMATION);
    result = 1;
  }
    catch (...) { guard::log(L"中键菜单出现未知异常"); result = 1; }
  return result;
}
}
