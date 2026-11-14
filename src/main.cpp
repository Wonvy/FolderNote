#include "core.h"
#include "language.h"
#include "context_commands.h"
#include "desktop_groups.h"
#include "desktop_groups_visibility.h"
#include "navigation.h"
#include "quick_folders.h"
#include <climits>
#include "explorer_refresh.h"
#include "resource.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <set>
#include <shellapi.h>
#include <thread>
#include <memory>
#include "window_position.h"
#define MessageBoxW ui::message
static HWND window, edit, statusText, saveButton, clearButton, heading, hint;
static HFONT font, headingFont;
static std::unique_ptr<ui::Theme> appearance;
static int operation;
static std::vector<std::wstring> paths;
static bool busy = false;
static std::wstring failures;
static UINT dpi = 96;
static int px(int v) { return MulDiv(v, dpi, 96); }
static void error(const std::wstring &s) {
  MessageBoxW(window, s.c_str(), L"FolderNote", MB_OK | MB_ICONERROR);
}
static void clipboard(const std::wstring &s) {
  if (!OpenClipboard(window))
    return;
  auto h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * 2);
  if (h) {
    auto p = GlobalLock(h);
    if (p) {
      memcpy(p, s.c_str(), (s.size() + 1) * 2);
      GlobalUnlock(h);
      EmptyClipboard();
      if (!SetClipboardData(CF_UNICODETEXT, h))
        GlobalFree(h);
    } else
      GlobalFree(h);
  }
  CloseClipboard();
}
static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR,
                                 DWORD_PTR) {
  if (m == WM_PASTE) {
    if (OpenClipboard(h)) {
      auto data = GetClipboardData(CF_UNICODETEXT);
      if (data) {
        auto p = (wchar_t *)GlobalLock(data);
        if (p) {
          std::wstring s = p;
          GlobalUnlock(data);
          for (auto &c : s)
            if (c == L'\r' || c == L'\n' || c == L'\t')
              c = L' ';
          SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
        }
      }
      CloseClipboard();
    }
    return 0;
  }
  return DefSubclassProc(h, m, w, l);
}
static void titleTheme() {
  if (appearance) { appearance->reset(ui::configuredTheme()); appearance->apply(window); }
}
static HWND control(const wchar_t *cls, const wchar_t *text, DWORD style,
                    int id) {
  auto h =
      CreateWindowExW(0, cls,
                      text, WS_CHILD | WS_VISIBLE | style | (cls == std::wstring(L"EDIT") ? WS_BORDER : 0), 0, 0, 0, 0, window,
                      (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
  return h;
}
static void layout() {
  RECT r;
  GetClientRect(window, &r);
  int width = r.right;
  MoveWindow(heading, px(24), px(14), width - px(48), px(30), TRUE);
  MoveWindow(statusText, px(24), px(52), width - px(48), px(42), TRUE);
  MoveWindow(edit, px(24), px(102), width - px(48), px(operation == 0 ? 32 : 110), TRUE);
  MoveWindow(hint, px(24), px(operation == 0 ? 146 : 222), width - px(48), px(22), TRUE);
  int y = operation == 0 ? 186 : 262;
  MoveWindow(saveButton, width - px(292), px(y), px(128), px(32), TRUE);
  MoveWindow(clearButton, width - px(152), px(y), px(128), px(32), TRUE);
}
struct Result {
  std::wstring summary, failures, saveError;
  bool single = false;
};
static void finishSave(bool clear) {
  if (busy)
    return;
  try {
    int len = GetWindowTextLengthW(edit);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(edit, s.data(), len + 1);
    s.resize(len);
    if (clear && MessageBoxW(window, lang::text(L"清除这个文件夹的备注？"), L"FolderNote",
                             MB_YESNO | MB_ICONQUESTION) != IDYES)
      return;
    busy = true;
    EnableWindow(saveButton, FALSE);
    EnableWindow(clearButton, FALSE);
    EnableWindow(edit, FALSE);
    SetWindowTextW(statusText, lang::text(L"正在保存备注并更新列表…"));
    std::thread([folder = paths[0], value = clear ? std::wstring{} : s, target = window] {
      auto r = std::make_unique<Result>();
      r->single = true;
      HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      try {
        fn::save(folder, value);
        if (SUCCEEDED(initialized))
          fn::refreshOpenViews({folder});
      } catch (const fn::Error &e) {
        r->saveError = e.message;
      } catch (...) {
        r->saveError = lang::text(L"保存失败，请检查路径和权限。");
      }
      if (SUCCEEDED(initialized))
        CoUninitialize();
      if (PostMessageW(target, WM_APP, 0, reinterpret_cast<LPARAM>(r.get())))
        r.release();
    }).detach();
  } catch (const fn::Error &e) {
    error(e.message);
  } catch (...) {
    busy = false;
    EnableWindow(saveButton, TRUE);
    EnableWindow(clearButton, TRUE);
    EnableWindow(edit, TRUE);
    SetWindowTextW(statusText, paths[0].c_str());
    error(lang::text(L"保存失败，请检查路径和权限。"));
  }
}
static void runBatch() {
  busy = true;
  EnableWindow(saveButton, FALSE);
  EnableWindow(clearButton, FALSE);
  std::thread([] {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto r = new Result;
    int repaired = 0, normal = 0, missingFile = 0, missingNote = 0, failed = 0;
    std::set<std::wstring> seen;
    std::vector<std::wstring> changed;
    for (auto &p : paths) {
      if (!seen.insert(p).second)
        continue;
      try {
        if (operation == 1) {
          fn::save(p, L"");
          changed.push_back(p);
          ++repaired;
        } else {
          switch (fn::repair(p)) {
          case fn::Status::Repaired:
            changed.push_back(p);
            ++repaired;
            break;
          case fn::Status::Normal:
            changed.push_back(p);
            ++normal;
            break;
          case fn::Status::MissingFile:
            ++missingFile;
            break;
          case fn::Status::MissingNote:
            ++missingNote;
            break;
          }
        }
      } catch (const fn::Error &e) {
        ++failed;
        r->failures += p + L"\r\n" + e.message + L"\r\n\r\n";
      } catch (...) {
        ++failed;
        r->failures += lang::format(L"{0}：处理失败。", {p}) + L"\r\n";
      }
    }
    if (SUCCEEDED(initialized)) {
      fn::refreshOpenViews(changed);
      CoUninitialize();
    }
    r->summary = lang::format(
        operation == 1 ? L"已处理 {0}，原本正常 {1}\r\n无备注文件 {2}，无备注 {3}，失败 {4}"
                       : L"已恢复 {0}，原本正常 {1}\r\n无备注文件 {2}，无备注 {3}，失败 {4}",
        {std::to_wstring(repaired), std::to_wstring(normal), std::to_wstring(missingFile),
         std::to_wstring(missingNote), std::to_wstring(failed)});
    if (!PostMessageW(window, WM_APP, 0, (LPARAM)r))
      delete r;
  }).detach();
}
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == ui::themeChangedMessage()) { titleTheme(); return 0; }
  LRESULT themed = 0;
  if (appearance && appearance->handle(h, m, w, l, themed)) return themed;
  switch (m) {
  case WM_CREATE: {
    window = h;
    dpi = GetDpiForWindow(h);
    font = ui::makeFont(dpi); headingFont = ui::makeFont(dpi, 21, FW_SEMIBOLD);
    appearance = std::make_unique<ui::Theme>();
    heading = control(L"STATIC", lang::text(operation == 0 ? L"文件夹备注" : L"批量处理"), SS_LEFT, 20);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    hint = control(L"STATIC", lang::text(operation == 0 ? L"最多 500 个字符 · Enter 保存 · Esc 取消" :
                                                        L"仅处理选中的文件夹，不会扫描子目录"), SS_LEFT, 21);
    ui::muted(hint);
    statusText = control(L"STATIC",
                         operation == 0 ? paths[0].c_str()
                                        : lang::text(L"正在处理所选文件夹，请稍候…"),
                         SS_LEFT, 10);
    ui::muted(statusText);
    edit = control(L"EDIT", L"",
                   WS_TABSTOP | (operation == 0
                                     ? ES_AUTOHSCROLL
                                     : ES_MULTILINE | ES_READONLY | WS_VSCROLL),
                   11);
    saveButton = control(L"BUTTON", lang::text(operation == 0 ? L"保存" : L"复制失败项"),
                         WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    clearButton = control(L"BUTTON", lang::text(operation == 0 ? L"清除备注" : L"关闭"),
                          WS_TABSTOP, 12);
    titleTheme();
    if (operation == 0) {
      SendMessageW(edit, EM_SETLIMITTEXT, 1000, 0);
      SetWindowSubclass(edit, editProc, 1, 0);
      try {
        auto s = fn::note(paths[0]);
        SetWindowTextW(edit, s.c_str());
      } catch (const fn::Error &e) {
        PostMessageW(h, WM_CLOSE, 0, 0);
        error(e.message);
      }
      SetFocus(edit);
      SendMessageW(edit, EM_SETSEL, 0, -1);
    } else
      runBatch();
    layout();
    return 0;
  }
  case WM_SIZE:
    layout();
    return 0;
  case WM_SETTINGCHANGE: case WM_THEMECHANGED:
    titleTheme();
    return 0;
  case WM_DPICHANGED: {
    dpi = HIWORD(w);
    auto r = (RECT *)l;
    SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left,
                 r->bottom - r->top, SWP_NOZORDER);
    auto old = font, oldHeading = headingFont;
    font = ui::makeFont(dpi); headingFont = ui::makeFont(dpi, 21, FW_SEMIBOLD);
    for (auto c : {edit, statusText, saveButton, clearButton, hint})
      SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    DeleteObject(old); DeleteObject(oldHeading);
    layout();
    return 0;
  }
  case WM_COMMAND:
    if (LOWORD(w) == IDOK) {
      if (operation == 0)
        finishSave(false);
      else
        clipboard(failures);
    } else if (LOWORD(w) == 12) {
      if (operation == 0)
        finishSave(true);
      else if (!busy)
        DestroyWindow(h);
    }
    return 0;
  case WM_APP: {
    auto r = (Result *)l;
    busy = false;
    if (r->single) {
      auto message = r->saveError;
      delete r;
      if (message.empty()) {
        DestroyWindow(h);
      } else {
        EnableWindow(saveButton, TRUE);
        EnableWindow(clearButton, TRUE);
        EnableWindow(edit, TRUE);
        SetWindowTextW(statusText, paths[0].c_str());
        error(message);
        SetFocus(edit);
      }
      return 0;
    }
    failures = r->failures;
    SetWindowTextW(statusText, r->summary.c_str());
    SetWindowTextW(edit, failures.empty() ? lang::text(L"处理完成。") : failures.c_str());
    EnableWindow(saveButton, !failures.empty());
    EnableWindow(clearButton, TRUE);
    delete r;
    return 0;
  }
  case WM_CLOSE:
    if (!busy)
      DestroyWindow(h);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  case WM_NCDESTROY:
    DeleteObject(font); DeleteObject(headingFont); appearance.reset(); break;
  }
  return DefWindowProcW(h, m, w, l);
}
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int argc = 0;
  auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argc == 3 && std::wstring_view(argv[1]) == L"--desktop-groups-watch") {
    wchar_t *end = nullptr;
    const DWORD pid = wcstoul(argv[2], &end, 10);
    const bool valid = pid && end && !*end;
    LocalFree(argv);
    const auto result = valid ? desktop_groups::watchDesktopIcons(pid) : 1;
    CoUninitialize();
    return result;
  }
  if (argc == 2 && (std::wstring_view(argv[1]) == L"--desktop-groups" ||
                    std::wstring_view(argv[1]) == L"--desktop-groups-manage" ||
                    std::wstring_view(argv[1]) == L"--desktop-groups-create")) {
    const bool manage = std::wstring_view(argv[1]) == L"--desktop-groups-manage";
    const bool create = std::wstring_view(argv[1]) == L"--desktop-groups-create";
    LocalFree(argv);
    const auto result = desktop_groups::run(inst, manage, create);
    CoUninitialize();
    return result;
  }
  if (argc == 4 && std::wstring_view(argv[1]) == L"--context-command") {
    int result = 1;
    try { result = context::runCommand(argv[2], argv[3]); }
    catch (const fn::Error &error) {
      MessageBoxW(nullptr, error.message.c_str(), L"FolderNote", MB_OK | MB_ICONERROR);
    } catch (...) {
      MessageBoxW(nullptr, lang::text(L"无法执行右键菜单命令。"), L"FolderNote", MB_OK | MB_ICONERROR);
    }
    LocalFree(argv); CoUninitialize(); return result;
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--quick-helper") {
    LocalFree(argv); auto result = quick::helper(inst); CoUninitialize(); return result;
  }
  if (argc == 2 && std::wstring(argv[1]) == L"--quick-manage") {
    LocalFree(argv); auto result = navigation::settings(inst, navigation::SettingsPage::QuickFolders); CoUninitialize(); return result;
  }
  if (argc == 6 && std::wstring(argv[1]) == L"--quick-menu") {
    int result = 1;
    try {
      auto number = [&](int index) { size_t used = 0; auto value = std::stoll(argv[index], &used); if (used != wcslen(argv[index])) throw 1; return value; };
      auto target = number(2), view = number(3), x = number(4), y = number(5);
      if (target <= 0 || view < 0 || x < LONG_MIN || x > LONG_MAX || y < LONG_MIN || y > LONG_MAX) throw 1;
      result = quick::menu(inst, reinterpret_cast<HWND>(static_cast<UINT_PTR>(target)),
                            reinterpret_cast<HWND>(static_cast<UINT_PTR>(view)), {static_cast<LONG>(x), static_cast<LONG>(y)});
    } catch (...) { }
    LocalFree(argv); CoUninitialize(); return result;
  }
  if(argc==1 || (argc==2 && std::wstring(argv[1])==L"--settings")) {
    LocalFree(argv);int result=navigation::settings(inst);CoUninitialize();return result;
  }
  if(argc==2 && (std::wstring(argv[1])==L"--listen" || std::wstring(argv[1])==L"--stop-listener")) {
    // Compatibility with old shortcuts: retire the old listener, never recreate it.
    LocalFree(argv);navigation::stopLegacy();CoUninitialize();return 0;
  }
  try {
    if (argc == 3 && std::wstring(argv[1]) == L"--request")
      fn::readRequest(argv[2], operation, paths);
    else if (argc >= 3) {
      std::wstring op = argv[1];
      if (op == L"--edit")
        operation = 0;
      else if (op == L"--clear")
        operation = 1;
      else if (op == L"--repair")
        operation = 2;
      else
        throw fn::Error{lang::text(L"未知命令。")};
      for (int i = 2; i < argc; ++i)
        paths.push_back(argv[i]);
    } else {
      MessageBoxW(
          nullptr,
          lang::text(L"请在文件夹上右键选择“添加/"
          L"编辑备注”或“修复文件夹备注”。\n\n首次使用请运行 Install.exe "
          L"安装右键菜单。\n资源管理器切换到详细信息视图，右键列标题并勾选“备注"
          L"”。"),
          L"FolderNote", MB_OK);
      LocalFree(argv);
      CoUninitialize();
      return 0;
    }
    if (!navigation::notesEnabled())
      throw fn::Error{lang::text(L"文件夹备注功能已关闭。请在 FolderNote 设置中开启后重试。")};
    if (paths.empty() || (operation == 0 && paths.size() != 1))
      throw fn::Error{lang::text(L"编辑备注时请选择一个文件夹。")};
    if (operation == 1 &&
        MessageBoxW(nullptr,
                    lang::format(L"清除所选 {0} 个文件夹的备注？", {std::to_wstring(paths.size())}).c_str(),
                    L"FolderNote", MB_YESNO | MB_ICONQUESTION) != IDYES) {
      LocalFree(argv);
      CoUninitialize();
      return 0;
    }
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_FOLDERNOTE));
    wc.lpszClassName = L"FolderNoteWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    dpi = GetDpiForSystem();
    const RECT targetWork=ui::mouseWorkArea();
    window = CreateWindowExW(
        WS_EX_CONTROLPARENT, wc.lpszClassName,
        lang::text(operation == 0 ? L"文件夹备注 — FolderNote" : L"批量处理 — FolderNote"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, targetWork.left,
        targetWork.top, px(580), px(operation == 0 ? 278 : 354), nullptr,
        nullptr, inst, nullptr);
    if (!window)
      fn::fail(L"无法打开窗口");
    ui::centerLogical(window,targetWork,580,operation==0?278:354);
    ShowWindow(window, SW_SHOW);
    if (operation == 0)
      SetFocus(edit);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
        SendMessageW(window, WM_CLOSE, 0, 0);
        continue;
      }
      if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
          operation == 0) {
        finishSave(false);
        continue;
      }
      if (!IsDialogMessageW(window, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  } catch (const fn::Error &e) {
    error(e.message);
  } catch (...) {
    error(lang::text(L"无法完成操作。"));
  }
  LocalFree(argv);
  CoUninitialize();
  return 0;
}
