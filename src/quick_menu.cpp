#include "quick_folders.h"
#include "quick_popup.h"
#include "quick_menu_gate.h"
#include "navigation.h"
#include "core.h"
#include "window_position.h"
#include <UIAutomation.h>
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
    if (FAILED(dispatch.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)))) continue;
    ComPtr<IShellView> view; HWND viewWindow = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view || FAILED(view->GetWindow(&viewWindow)) ||
        !IsWindowVisible(viewWindow) || (expectedView && expectedView != viewWindow) ||
        GetAncestor(viewWindow, GA_ROOT) != target) continue;
    if (actualView) *actualView = viewWindow;
    return browser;
  }
  return {};
}
std::wstring pathFromText(const std::wstring &text) {
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    if ((text[i + 1] == L':' && text[i + 2] == L'\\') || (text[i] == L'\\' && text[i + 1] == L'\\')) {
      auto path = normalize(text.substr(i));
      if (!path.empty()) return path;
    }
  }
  return {};
}
std::wstring dialogPath(HWND target) {
  ComPtr<IUIAutomation> automation;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return {};
  ComPtr<IUIAutomation2> options;
  if (SUCCEEDED(automation.As(&options))) { options->put_ConnectionTimeout(200); options->put_TransactionTimeout(200); }
  ComPtr<IUIAutomationElement> root;
  ComPtr<IUIAutomationCondition> condition;
  VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ToolBarControlTypeId;
  if (FAILED(automation->ElementFromHandle(target, &root)) ||
      FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition))) return {};
  ComPtr<IUIAutomationElementArray> bars;
  if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &bars))) return {};
  int count = 0; bars->get_Length(&count);
  for (int i = 0; i < std::min(count, 16); ++i) {
    ComPtr<IUIAutomationElement> element; BSTR name = nullptr;
    if (SUCCEEDED(bars->GetElement(i, &element)) && SUCCEEDED(element->get_CurrentName(&name))) {
      auto path = name ? pathFromText(name) : std::wstring(); SysFreeString(name);
      if (!path.empty()) return path;
    }
  }
  return {};
}
bool navigateDialog(HWND target, const std::wstring &path, const detail::MenuRequest &request) {
  if (!request.current() || !isFileDialog(target)) return false;
  SetForegroundWindow(target);
  if (!request.current() || GetForegroundWindow() != target ||
      ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU)) & 0x8000)) return false;
  INPUT keys[4]{};
  for (auto &key : keys) key.type = INPUT_KEYBOARD;
  keys[0].ki.wVk = VK_MENU; keys[1].ki.wVk = L'D';
  keys[2].ki.wVk = L'D'; keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
  keys[3].ki.wVk = VK_MENU; keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
  if (SendInput(4, keys, sizeof(INPUT)) != 4) { SendInput(2, keys + 2, sizeof(INPUT)); return false; }
  DWORD pid = 0; const auto thread = GetWindowThreadProcessId(target, &pid);
  HWND edit = nullptr;
  for (int i = 0; i < 50; ++i) {
    if (!request.current() || GetForegroundWindow() != target || !isFileDialog(target)) return false;
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(thread, &info) && addressEdit(info.hwndFocus, target)) { edit = info.hwndFocus; break; }
    Sleep(10);
  }
  if (!edit || !request.current()) return false;
  DWORD_PTR result = 0;
  if (!SendMessageTimeoutW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(path.c_str()), SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result) || !result)
    return false;
  GUITHREADINFO info{sizeof(info)}; DWORD currentPid = 0;
  if (!request.current() || GetForegroundWindow() != target || !GetGUIThreadInfo(thread, &info) || info.hwndFocus != edit ||
      !addressEdit(edit, target) || !GetWindowThreadProcessId(target, &currentPid) || currentPid != pid) return false;
  // Deliver Enter directly to the verified address edit, never the filename
  // control, dialog default button, or whatever application gains focus next.
  if (!SendMessageTimeoutW(edit, WM_KEYDOWN, VK_RETURN, 1, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result)) return false;
  SendMessageTimeoutW(edit, WM_KEYUP, VK_RETURN, 0xc0000001, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &result);
  return true;
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
enum class Kind { Open, NewWindow, Pin, Unpin, Manage, Clear };
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
    add(*children, L"在新窗口打开", Kind::NewWindow, Symbol::Window, path);
    add(*children, pinned(path) ? L"取消置顶" : L"置顶此文件夹", pinned(path) ? Kind::Unpin : Kind::Pin, pinned(path) ? Symbol::Unpin : Symbol::Star, path);
    separator(*children);
    if (depth >= 8) hint(*children, L"更深的目录请直接打开");
    else {
      hint(*children, L"正在读取子文件夹…");
      children->poll = [this, path, depth, listing = std::shared_ptr<Scan>{}](Branch &target) mutable {
        if (!listing) { listing = scan(path); return false; }
        std::unique_lock lock(listing->mutex);
        if (!listing->done) return false;
        target.items.resize(3);
        if (listing->error && listing->error != ERROR_FILE_NOT_FOUND) hint(target, L"目录暂不可用或无权访问");
        else if (listing->folders.empty()) hint(target, L"没有子文件夹");
        else for (const auto &child : listing->folders) folder(target, child, depth + 1);
        if (listing->more) hint(target, L"更多内容请打开此文件夹");
        return true;
      };
    }
    auto title = label(path); auto symbol = Symbol::Folder;
    for (const auto &value : known) if (same(value.path, path)) { title = value.title; symbol = value.symbol; break; }
    branch.items.push_back({title, command(Kind::Open, path), symbol, children, Row::Action, path});
  }
  void build(const std::wstring &current) {
    state = load();
    const KNOWNFOLDERID *ids[]{&FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads};
    const wchar_t *names[]{L"桌面", L"文档", L"下载"};
    const Symbol symbols[]{Symbol::Desktop, Symbol::Documents, Symbol::Downloads};
    for (size_t i = 0; i < 3; ++i) {
      PWSTR path = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(*ids[i], KF_FLAG_DONT_VERIFY, nullptr, &path))) { known.push_back({path, names[i], symbols[i]}); CoTaskMemFree(path); }
    }
    heading(*root, L"置顶文件夹");
    if (!current.empty()) root->items.back().actions.push_back({command(pinned(current) ? Kind::Unpin : Kind::Pin, current), pinned(current) ? Symbol::Unpin : Symbol::Star,
        pinned(current) ? L"取消置顶当前文件夹" : L"置顶当前文件夹"});
    if (state.pins.empty()) hint(*root, L"暂无置顶");
    for (const auto &path : state.pins) folder(*root, path);
    separator(*root); heading(*root, L"最近访问");
    unsigned shown = 0;
    for (const auto &path : state.history) if (!pinned(path) && shown < 8) { folder(*root, path); ++shown; }
    if (!shown) hint(*root, L"暂无记录");
    auto history = std::make_shared<Branch>();
    heading(*history, L"历史记录 · 最近 20 条");
    for (const auto &path : state.history) folder(*history, path);
    if (state.history.empty()) hint(*history, L"暂无历史记录");
    root->items.push_back({L"历史记录（最近 20 条）", 0, Symbol::More, history});
    separator(*root);
    Item toolbar; toolbar.row = Row::Toolbar;
    toolbar.actions = {{command(Kind::Manage), Symbol::Settings, L"管理置顶与历史"}, {command(Kind::Clear), Symbol::Trash, L"清空历史"}};
    root->items.push_back(std::move(toolbar));
  }
};
}
int menu(HINSTANCE instance, HWND target, HWND view, POINT point) {
  if (!navigation::quickEnabled() || (!isExplorer(target) && !isFileDialog(target))) return 1;
  if (GetForegroundWindow() != target) return 0;
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
    if (!request.valid()) fn::fail(L"无法准备快速文件夹菜单", request.error());
    std::wstring current;
    bool repeat = true;
    while (repeat && navigation::quickEnabled()) {
      repeat = false;
      Popup popup;
      UINT selected = 0;
      {
        detail::MenuGate gate;
        if (!gate.acquire()) {
          if (gate.error() != ERROR_TIMEOUT) fn::fail(L"无法打开快速文件夹菜单", gate.error());
          break;
        }
        if (!validTarget() || GetForegroundWindow() != target || !navigation::quickEnabled()) break;
        if (!started) {
          request.begin(); started = true;
          // Keep preparation serialized too: a slow Shell provider must not
          // accumulate one blocked query process per repeated middle click.
          if (isExplorer(target)) {
            auto browser = browserFor(target, view, &view);
            ComPtr<IShellView> active;
            if (browser && SUCCEEDED(browser->QueryActiveShellView(&active))) current = viewPath(active.Get());
          } else current = dialogPath(target);
          if (!current.empty()) visit(current);
        }
        if (!currentRequest() || GetForegroundWindow() != target) break;
        popup.build(current);
        // Slow queries must not reopen after focus changes or a newer request.
        if (!currentRequest() || GetForegroundWindow() != target) break;
        selected = popup::show(instance, point, popup.root, ui::configuredTheme(), target);
      } // The hidden popup no longer owns the gate, even if its command is slow.
      if (!selected || !popup.commands.contains(selected) || !currentRequest()) break;
      auto command = popup.commands.at(selected);
      if (command.kind == Kind::Pin || command.kind == Kind::Unpin) {
        pin(command.path, command.kind == Kind::Pin); repeat = true;
      } else if (command.kind == Kind::Manage) manage(instance);
      else if (command.kind == Kind::Clear) {
        const auto answer = ui::message(nullptr, L"清空访问历史？置顶文件夹将保留。", L"快速文件夹", MB_YESNO | MB_ICONQUESTION);
        if (!currentRequest()) break;
        if (answer == IDYES) clearHistory();
        repeat = true;
      } else {
        const bool available = directoryAvailable(command.path);
        if (!currentRequest()) break;
        if (!available) throw fn::Error{L"该文件夹不存在、无权访问或响应超时。置顶记录已保留。"};
        PIDLIST_ABSOLUTE parsed = nullptr;
        const auto parsedResult = SHParseDisplayName(command.path.c_str(), nullptr, &parsed, 0, nullptr);
        struct ParsedId {
          PIDLIST_ABSOLUTE value;
          ~ParsedId() { CoTaskMemFree(value); }
          PIDLIST_ABSOLUTE get() const noexcept { return value; }
        } id{parsed};
        if (!currentRequest()) break;
        if (FAILED(parsedResult)) throw fn::Error{L"无法打开这个文件夹。"};
        bool ok = false;
        if (command.kind == Kind::NewWindow) {
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
        } else ok = navigateDialog(target, command.path, request);
        if (!currentRequest()) break;
        if (!ok) throw fn::Error{L"未能在原窗口定位文件夹。请确认窗口或标签页未切换，且程序未以管理员权限运行。\n不支持的自定义文件对话框可使用“在新窗口打开”。"};
        visit(command.path);
      }
    }
  } catch (const fn::Error &error) {
    // A late failure from an obsolete request must not steal focus back.
    if ((!started || request.current()) && navigation::quickEnabled() && validTarget())
      ui::message(nullptr, error.message.c_str(), L"快速文件夹", MB_OK | MB_ICONINFORMATION);
    result = 1;
  }
    catch (...) { result = 1; }
  return result;
}
}
