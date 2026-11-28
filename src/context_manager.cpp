#include "context_manager.h"
#include "context_commands.h"
#include "context_inventory.h"
#include "quick_find.h"
#include "quick_find_controls.h"
#include "context_order.h"
#include "core.h"
#include "language.h"
#include "navigation.h"
#include "quick_folders.h"
#include "resource.h"
#include "theme.h"
#include "theme_scroll.h"
#include "system_icon_picker.h"
#include "window_position.h"
#include <algorithm>
#include <array>
#include <iterator>
#include <future>
#include <thread>
#include <chrono>
#include <atomic>
#include <utility>
#include <span>
#include <set>
#include <map>
#include <string_view>
#include <shellapi.h>
#include <shlwapi.h>
#include <oleacc.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <windowsx.h>

namespace contextmenu {
namespace {
using context::Command;
using context::ExistingItem;
using context::Mode;
using context::Scene;

constexpr auto title = L"右键菜单管理";
constexpr LPARAM directEyeClick = 1;
constexpr wchar_t managerClass[] = L"ShibaKit.ContextMenu.Manager";
constexpr wchar_t editorClass[] = L"ShibaKit.ContextMenu.Editor";
constexpr std::array scenes{Scene::File, Scene::Extension, Scene::Folder, Scene::FolderBackground,
    Scene::Drive, Scene::Desktop, Scene::New, Scene::SendTo, Scene::OpenWith};
constexpr std::array sceneLabels{L"所有文件", L"指定文件扩展名", L"文件夹", L"文件夹空白处",
    L"驱动器", L"桌面空白处", L"新建", L"发送到", L"打开方式"};
constexpr std::array modes{L"程序", L"CMD", L"PowerShell", L"网址"};

enum Id {
  enable = 100, category, extension, refresh, originals, commands, details,
  visibility, appearance, restore, restoreAll, copyCommand, add, edit, remove, toggle, up, down,
  originalLabel, commandLabel, explanation, registryLocation = explanation + 2,
  categoryScroll, originalsScroll, commandsScroll, nameColumn, statusColumn, menuSearch, pageHeading, extensionDrop
};
enum EditorId { entryMode = 200, entryName, entryProgram, entryArguments, entryDirectory, entryPath, entryIcon, descendants, preview,
  modeLabel, nameLabel, programLabel, argumentsLabel, directoryLabel, pathLabel, iconLabel, previewLabel,
  browseDirectory, browsePath, browseIcon };

std::wstring windowText(HWND control) {
  std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
  value.resize(static_cast<size_t>(GetWindowTextW(control, value.data(), static_cast<int>(value.size()))));
  return value;
}
int index(HWND window, int id, UINT message = LB_GETCURSEL) {
  return static_cast<int>(SendDlgItemMessageW(window, id, message, 0, 0));
}
void append(HWND control, const std::wstring &value, UINT message = LB_ADDSTRING) {
  SendMessageW(control, message, 0, reinterpret_cast<LPARAM>(value.c_str()));
}
std::wstring suffix(HWND window) {
  auto value = windowText(GetDlgItem(window, extension));
  if (!value.empty() && value.front() != L'.') value.insert(value.begin(), L'.');
  for (auto &ch : value) if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + L'a' - L'A');
  return value;
}

struct Window {
  HWND window = nullptr;
  HINSTANCE instance;
  ui::Theme theme;
  HFONT font = nullptr;
  UINT dpi = 96;
  explicit Window(HINSTANCE value) : instance(value) { theme.reset(ui::configuredTheme()); }
  ~Window() { DeleteObject(font); }
  int px(int value) const { return MulDiv(value, dpi, 96); }
  HWND control(int id) const { return GetDlgItem(window, id); }
  HWND create(int id, const wchar_t *type, const wchar_t *label, DWORD style, DWORD extended = 0) {
    HWND child = CreateWindowExW(extended, type, lang::text(label), WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
    if (!child) fn::fail(L"无法创建右键菜单管理窗口。");
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return child;
  }
  void button(int id, const wchar_t *label, DWORD style = 0) {
    create(id, L"BUTTON", label, WS_TABSTOP | style);
  }
  void field(int id, DWORD limit, DWORD style = ES_AUTOHSCROLL) {
    auto child = create(id, L"EDIT", L"", WS_TABSTOP | style, WS_EX_CLIENTEDGE);
    SendMessageW(child, EM_SETLIMITTEXT, limit, 0);
  }
  void combo(int id) { create(id, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST); }
  void set(int id, const wchar_t *value) { SetWindowTextW(control(id), value); }
  void label(int id, const wchar_t *value) { create(id, L"STATIC", value, SS_NOPREFIX); }
  void bounds(int id, int x, int y, int width, int height) {
    if (auto child = control(id)) MoveWindow(child, x, y, std::max(0, width), std::max(0, height), TRUE);
  }
  void scaled(int id, int x, int y, int width, int height) {
    bounds(id, px(x), px(y), px(width), px(height));
  }
  void enableControl(int id, bool value) { EnableWindow(control(id), value); }
  void rescale(UINT value) {
    if (value == dpi && font) return;
    dpi = value; auto old = font; font = ui::makeFont(dpi);
    EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL {
      SendMessageW(child, WM_SETFONT, value, TRUE); return TRUE;
    }, reinterpret_cast<LPARAM>(font));
    DeleteObject(old);
  }
  void init() { rescale(GetDpiForWindow(window)); }
  void updateTheme() { theme.reset(ui::configuredTheme()); theme.apply(window); }
};

void modal(HWND window, HWND owner, int width, int height, bool center = true, HWND dropdownButton = nullptr) {
  struct RestoreOwner {
    HWND window;
    bool enabled;
    ~RestoreOwner() {
      if (enabled && IsWindow(window)) { EnableWindow(window, TRUE); SetActiveWindow(window); }
    }
  } restoreOwner{owner, !dropdownButton && owner && IsWindowEnabled(owner)};
  if (restoreOwner.enabled) EnableWindow(owner, FALSE);
  if (center) ui::centerLogical(window, ui::mouseWorkArea(), width, height);
  ShowWindow(window, SW_SHOWNORMAL);
  MSG message{};
  while (IsWindow(window)) {
    const int result = GetMessageW(&message, nullptr, 0, 0);
    if (result <= 0) {
      DestroyWindow(window);
      if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    const bool outside = message.hwnd != window && !IsChild(window, message.hwnd);
    if (dropdownButton && outside &&
        (message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONDOWN ||
         message.message == WM_MBUTTONDOWN || message.message == WM_NCLBUTTONDOWN)) {
      DestroyWindow(window);
      if (message.hwnd != dropdownButton) DispatchMessageW(&message);
      break;
    }
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
        (message.hwnd == window || IsChild(window, message.hwnd))) {
      if (SendMessageW(message.hwnd, CB_GETDROPPEDSTATE, 0, 0)) SendMessageW(message.hwnd, CB_SHOWDROPDOWN, FALSE, 0);
      else DestroyWindow(window);
    } else if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
}

struct Editor : Window {
  Command command;
  bool rename, accepted = false;
  Editor(HINSTANCE instance, Command value, bool rename) : Window(instance), command(std::move(value)), rename(rename) {}
  void field(int id, DWORD limit, DWORD style = ES_AUTOHSCROLL) {
    auto child = create(id, L"EDIT", L"", WS_TABSTOP | style);
    SendMessageW(child, EM_SETLIMITTEXT, limit, 0);
  }
  void selectFolder(int id) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    auto result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    FILEOPENDIALOGOPTIONS flags{};
    if (SUCCEEDED(result)) result = dialog->GetOptions(&flags);
    if (SUCCEEDED(result)) result = dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(result)) throw fn::Error{lang::text(L"无法打开文件夹选择窗口。")};
    const auto initial = windowText(control(id));
    if (!initial.empty()) {
      Microsoft::WRL::ComPtr<IShellItem> start;
      if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&start))))
        dialog->SetFolder(start.Get());
    }
    result = dialog->Show(window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    Microsoft::WRL::ComPtr<IShellItem> folder;
    if (SUCCEEDED(result)) result = dialog->GetResult(&folder);
    PWSTR raw = nullptr;
    if (SUCCEEDED(result)) result = folder ? folder->GetDisplayName(SIGDN_FILESYSPATH, &raw) : E_FAIL;
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
    if (FAILED(result) || !path) throw fn::Error{lang::text(L"无法打开文件夹选择窗口。")};
    set(id, path.get());
  }
  void paint() const {
    PAINTSTRUCT info{};
    auto dc = BeginPaint(window, &info);
    FillRect(dc, &info.rcPaint, theme.background);
    for (int id : {entryName, entryProgram, entryArguments, entryDirectory, entryPath, entryIcon, preview}) {
      if (auto child = control(id)) {
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
        InflateRect(&bounds, px(7), px(4));
        ui::rounded(dc, bounds, theme.colors.field, theme.colors.border, px(5));
      }
    }
    EndPaint(window, &info);
  }
  Mode mode() const {
    return static_cast<Mode>(std::clamp(index(window, entryMode, CB_GETCURSEL), 0, 3));
  }
  Command edited() const {
    auto value = command;
    value.name = windowText(control(entryName));
    value.program = windowText(control(entryProgram));
    if (!rename) {
      value.mode = mode();
      value.arguments = windowText(control(entryArguments));
      value.directory = windowText(control(entryDirectory));
      value.pathFilter = windowText(control(entryPath));
      value.icon = windowText(control(entryIcon));
      value.includeChildren = SendDlgItemMessageW(window, descendants, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    return value;
  }
  void updatePreview() {
    if (rename || !control(preview)) return;
    const auto selected = mode();
    const bool program = selected == Mode::Program;
    const bool url = selected == Mode::Url;
    set(programLabel, lang::text(url ? L"网址" : L"程序路径"));
    set(argumentsLabel, lang::text(program ? L"参数" : L"命令"));
    enableControl(entryProgram, program || url);
    enableControl(entryArguments, !url);
    enableControl(entryDirectory, !url);
    enableControl(browseDirectory, !url);
    try { set(preview, context::previewCommand(edited()).c_str()); }
    catch (...) { set(preview, L""); }
  }
  void createControls() {
    label(nameLabel, L"菜单名称"); field(entryName, rename ? 256 : 120);
    set(entryName, command.name.c_str());
    if (rename) {
      label(programLabel, L"图标位置"); field(entryProgram, 2048); set(entryProgram, command.program.c_str());
      label(argumentsLabel, L"留空可还原对应项目的原始名称或图标。");
    } else {
      label(modeLabel, L"执行方式"); combo(entryMode);
      for (auto label : modes) append(control(entryMode), lang::text(label), CB_ADDSTRING);
      SendDlgItemMessageW(window, entryMode, CB_SETCURSEL, static_cast<WPARAM>(command.mode), 0);
      label(programLabel, L"程序路径"); field(entryProgram, 4096); set(entryProgram, command.program.c_str());
      label(argumentsLabel, L"参数");
      field(entryArguments, 8192);
      set(entryArguments, command.arguments.c_str());
      label(directoryLabel, L"工作目录"); field(entryDirectory, 4096); set(entryDirectory, command.directory.c_str());
      label(pathLabel, L"仅在此文件夹内显示"); field(entryPath, 4096); set(entryPath, command.pathFilter.c_str());
      label(iconLabel, L"图标位置"); field(entryIcon, 4096); set(entryIcon, command.icon.c_str());
      button(descendants, L"包括子文件夹", BS_AUTOCHECKBOX);
      SendDlgItemMessageW(window, descendants, BM_SETCHECK, command.includeChildren ? BST_CHECKED : BST_UNCHECKED, 0);
      label(previewLabel, L"执行预览 · 可用 {path} 和 {parent}");
      field(preview, 18000, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL);
      button(browseDirectory, L"选择文件夹");
      button(browsePath, L"选择文件夹");
      updatePreview();
    }
    button(browseIcon, L"选择图标");
    button(IDOK, L"保存", BS_DEFPUSHBUTTON);
    button(IDCANCEL, L"取消");
  }
  void layout() {
    scaled(nameLabel, 20, 18, 150, 26); scaled(entryName, 184, 20, 404, 20);
    scaled(programLabel, 20, rename ? 58 : 96, 150, 26);
    scaled(entryProgram, 184, rename ? 60 : 98, rename ? 292 : 404, 20);
    scaled(browseIcon, 492, rename ? 56 : 284, 104, 28);
    scaled(argumentsLabel, 20, rename ? 100 : 134, rename ? 576 : 150, 26);
    if (!rename) {
      scaled(modeLabel, 20, 57, 150, 26); scaled(entryMode, 176, 54, 420, 160);
      scaled(entryArguments, 184, 136, 404, 20);
      scaled(directoryLabel, 20, 174, 150, 26); scaled(entryDirectory, 184, 176, 292, 20);
      scaled(pathLabel, 20, 214, 150, 26); scaled(entryPath, 184, 216, 292, 20);
      scaled(browseDirectory, 492, 172, 104, 28);
      scaled(browsePath, 492, 212, 104, 28);
      scaled(descendants, 176, 248, 420, 28);
      scaled(iconLabel, 20, 286, 150, 26); scaled(entryIcon, 184, 288, 292, 20);
      scaled(previewLabel, 20, 326, 576, 26); scaled(preview, 28, 355, 560, 94);
    }
    scaled(IDOK, 374, rename ? 132 : 468, 106, 30);
    scaled(IDCANCEL, 490, rename ? 132 : 468, 106, 30);
  }
  void commit() {
    auto value = edited();
    if (!rename) {
      if (value.name.empty() || ((value.mode == Mode::Program || value.mode == Mode::Url)
            ? value.program.empty() : value.arguments.empty())) {
        ui::message(window, lang::text(L"请填写菜单名称和要执行的内容。"), lang::text(title), MB_OK | MB_ICONWARNING);
        return;
      }
      context::previewCommand(value);
    }
    command = std::move(value);
    accepted = true;
    DestroyWindow(window);
  }
};

LRESULT CALLBACK editorProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) {
    auto state = static_cast<Editor *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  auto *state = reinterpret_cast<Editor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return DefWindowProcW(window, message, w, l);
  try {
    if (message == WM_PAINT) { state->paint(); return 0; }
    if (message == WM_CTLCOLORSTATIC && reinterpret_cast<HWND>(l) == state->control(preview)) {
      auto dc = reinterpret_cast<HDC>(w);
      SetBkColor(dc, state->theme.colors.field);
      SetTextColor(dc, state->theme.colors.text);
      return reinterpret_cast<LRESULT>(state->theme.field);
    }
    LRESULT themed = 0;
    if (state->theme.handle(window, message, w, l, themed)) return themed;
    if (message == WM_CREATE) { state->init(); state->createControls(); state->updateTheme(); state->layout(); return 0; }
    if (message == ui::themeChangedMessage() || message == WM_THEMECHANGED) { state->updateTheme(); return 0; }
    if (message == WM_DPICHANGED) {
      state->rescale(HIWORD(w));
      const auto &rect = *reinterpret_cast<RECT *>(l);
      SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
      state->layout();
      return 0;
    }
    if (message == WM_COMMAND) {
      const auto id = LOWORD(w), code = HIWORD(w);
      if (code == BN_CLICKED && (id == browseDirectory || id == browsePath))
        state->selectFolder(id == browseDirectory ? entryDirectory : entryPath);
      if (code == BN_CLICKED && id == browseIcon) {
        const int field = state->rename ? entryProgram : entryIcon;
        if (auto icon = ui::pickSystemIcon(window, windowText(state->control(field))))
          state->set(field, icon->c_str());
      }
      if (id == IDOK && code == BN_CLICKED) state->commit();
      if (id == IDCANCEL && code == BN_CLICKED) DestroyWindow(window);
      if ((id == entryMode && code == CBN_SELCHANGE) ||
          ((id == entryProgram || id == entryArguments || id == entryDirectory) && code == EN_CHANGE)) state->updatePreview();
      return 0;
    }
  } catch (const fn::Error &error) {
    if (message == WM_CREATE) return -1;
    ui::message(window, error.message.c_str(), lang::text(title), MB_OK | MB_ICONERROR);
  } catch (...) {
    if (message == WM_CREATE) return -1;
    ui::message(window, lang::text(L"右键菜单操作失败。"), lang::text(title), MB_OK | MB_ICONERROR);
  }
  return DefWindowProcW(window, message, w, l);
}

bool editCommand(HINSTANCE instance, HWND owner, Command &command, bool rename = false) {
  WNDCLASSW cls{};
  cls.hInstance = instance; cls.lpszClassName = editorClass; cls.lpfnWndProc = editorProc;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&cls);
  Editor state(instance, command, rename);
  constexpr DWORD style = WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_CLIPCHILDREN;
  const auto monitor = ui::mouseWorkArea();
  HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, editorClass,
      lang::text(rename ? L"修改名称和图标" : L"编辑自定义命令"), style,
      monitor.left, monitor.top, 620, 560, owner, nullptr, instance, &state);
  if (!window) fn::fail(L"无法创建右键菜单管理窗口。");
  modal(window, owner, 634, rename ? 210 : 560);
  if (state.accepted) command = std::move(state.command);
  return state.accepted;
}

struct ExtensionPicker : Window {
  static constexpr int firstLetter = 501, list = 530;
  std::array<std::vector<std::wstring>, 26> extensions;
  std::wstring selected;
  wchar_t letter = L'A';
  ui::ScrollBar bar;
  explicit ExtensionPicker(HINSTANCE instance) : Window(instance) {}
  void fill(wchar_t initial) {
    SendDlgItemMessageW(window, firstLetter + letter - L'A', BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
    letter = initial;
    SendDlgItemMessageW(window, firstLetter + letter - L'A', BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
    SendDlgItemMessageW(window, list, WM_SETREDRAW, FALSE, 0);
    SendDlgItemMessageW(window, list, LB_RESETCONTENT, 0, 0);
    for (const auto &value : extensions[initial - L'A']) append(control(list), value);
    SendDlgItemMessageW(window, list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(control(list), nullptr, FALSE);
    RECT rect{}; GetClientRect(control(list), &rect);
    const int rowHeight = std::max(1, int(SendDlgItemMessageW(window, list, LB_GETITEMHEIGHT, 0, 0)));
    const int rows = std::max(1, int(rect.bottom) / rowHeight);
    const int count = int(SendDlgItemMessageW(window, list, LB_GETCOUNT, 0, 0));
    bar.configure((count + rows - 1) / rows, 4, 0, dpi);
  }
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
      auto self = static_cast<ExtensionPicker *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
      self->window = window;
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto self = reinterpret_cast<ExtensionPicker *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, w, l);
    if (message == WM_ACTIVATEAPP && !w) {
      DestroyWindow(window);
      return 0;
    }
    LRESULT themed{};
    if (self->theme.handle(window, message, w, l, themed)) return themed;
    if (message == WM_CREATE) {
      self->init();
      for (int i = 0; i < 26; ++i) {
        wchar_t letter[]{static_cast<wchar_t>(L'A' + i), 0};
        self->button(firstLetter + i, letter);
        self->scaled(firstLetter + i, 8 + (i % 9) * 69, 8 + (i / 9) * 31, 64, 28);
      }
      self->create(list, L"LISTBOX", L"", WS_TABSTOP | LBS_MULTICOLUMN | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT);
      self->scaled(list, 8, 104, 624, 204);
      SendDlgItemMessageW(window, list, LB_SETCOLUMNWIDTH, self->px(156), 0);
      self->bar.create(window, 531, self->theme, [self](int column) {
        RECT rect{}; GetClientRect(self->control(list), &rect);
        const int rowHeight = std::max(1, int(SendDlgItemMessageW(self->window, list, LB_GETITEMHEIGHT, 0, 0)));
        SendDlgItemMessageW(self->window, list, LB_SETTOPINDEX, column * std::max(1, int(rect.bottom) / rowHeight), 0);
      }, true, true);
      self->bar.move(self->px(8), self->px(309), self->px(624), self->px(10));
      self->fill(self->letter); self->updateTheme();
      return 0;
    }
    if (message == WM_COMMAND) {
      const auto id = LOWORD(w), code = HIWORD(w);
      if (id >= firstLetter && id < firstLetter + 26) {
        self->fill(static_cast<wchar_t>(L'A' + id - firstLetter));
      } else if (id == list && (code == LBN_SELCHANGE || code == LBN_DBLCLK)) {
        const auto row = SendDlgItemMessageW(window, list, LB_GETCURSEL, 0, 0);
        if (row != LB_ERR) {
          const auto length = SendDlgItemMessageW(window, list, LB_GETTEXTLEN, row, 0);
          if (length != LB_ERR) {
            self->selected.resize(static_cast<size_t>(length) + 1);
            SendDlgItemMessageW(window, list, LB_GETTEXT, row, reinterpret_cast<LPARAM>(self->selected.data()));
            self->selected.resize(static_cast<size_t>(length));
            DestroyWindow(window);
          }
        }
      }
      return 0;
    }
    return DefWindowProcW(window, message, w, l);
  }
};

struct Manager : Window {
  void (*setNoteHidden)(int, bool) = nullptr;
  wchar_t extensionLetter = L'A';
  enum class Divider { None, Category, Actions, Status };
  struct LoadResult {
    std::vector<ExistingItem> registered;
    std::vector<context::MenuPreviewItem> order, hidden;
    std::vector<Command> commands;
    bool enabled = false;
  };
  static constexpr UINT_PTR loadTimer = 301;
  static constexpr UINT_PTR registryTimer = 302;
  std::future<LoadResult> loading;
  std::future<DWORD> openingRegistry;
  std::shared_ptr<std::atomic<bool>> readingShell;
  bool reloadPending = false;
  int pendingOriginal = -1, pendingCustom = -1;
  bool active = false;
  bool customView = false;
  Scene scene = Scene::File;
  std::wstring currentExtension;
  std::vector<ExistingItem> existing;
  std::vector<ExistingItem> inventoryItems;
  std::map<size_t, size_t> submenuEnds;
  std::set<size_t> expandedMenus;
  std::vector<size_t> originalRows;
  std::vector<bool> childRows;
  std::vector<std::shared_ptr<std::remove_pointer_t<HICON>>> inventoryIcons;
  std::wstring searchText;
  std::vector<Command> custom;
  std::vector<size_t> displayed;
  std::vector<HICON> existingIcons, customIcons;
  std::array<ui::ScrollBar, 3> scrollbars;
  HWND tips = nullptr;
  HFONT symbols = nullptr;
  HFONT headingFont = nullptr;
  std::wstring tipText;
  std::wstring statusText;
  RECT categoryDivider{}, actionsDivider{}, statusDivider{};
  Divider dragging = Divider::None, hoveredDivider = Divider::None;
  int categoryWidth = 154, actionsWidth = 32, statusHeight = 23;
  explicit Manager(HINSTANCE instance) : Window(instance) {}
  ~Manager() { clearIcons(); DeleteObject(symbols); DeleteObject(headingFont); }

  static constexpr std::array<int, 6> existingActions{visibility, appearance, restore, copyCommand, registryLocation, details};
  static constexpr std::array<int, 6> customActions{add, edit, remove, toggle, up, down};
  static constexpr int refreshAction = refresh;

  int itemHeight(int id) const { return px(id == category ? 32 : 24); }
  int statusWidth() const { return px(120); }
  int visibilityAction(HWND list, POINT point) const {
    const int id = GetDlgCtrlID(list);
    if (id != originals && id != commands) return 0;
    const auto hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
    if (HIWORD(hit)) return 0;
    const auto row = LOWORD(hit);
    RECT rect{};
    if (SendMessageW(list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect)) == LB_ERR) return 0;
    const int left = rect.right - px(20) - statusWidth();
    rect.left = left - px(3);
    rect.right = left + px(23);
    if (!PtInRect(&rect, point)) return 0;
    if (id == commands) return row < displayed.size() ? toggle : 0;
    return row < existing.size() && existing[row].canHide ? visibility : 0;
  }
  void drawVisibility(HDC dc, RECT rect, bool shown, COLORREF color) const {
    auto old = SelectObject(dc, symbols);
    ui::text(dc, L"\xE890", rect, color,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    if (!shown) {
      const int middle = (rect.top + rect.bottom) / 2;
      auto pen = CreatePen(PS_SOLID, std::max(1, px(1)), color);
      if (pen) {
        auto oldPen = SelectObject(dc, pen);
        MoveToEx(dc, rect.left + px(2), middle + px(7), nullptr);
        LineTo(dc, rect.left + px(16), middle - px(7));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
      }
    }
  }
  static const wchar_t *status(const ExistingItem &entry) {
    if (entry.shellBlock && !entry.handlerRegistration && entry.hidden && entry.managed) return lang::text(L"已设为隐藏");
    return lang::text(entry.hidden ? L"已隐藏" : entry.canHide || entry.canEdit ? L"已显示" : L"只读");
  }
  static const wchar_t *status(const Command &entry) { return lang::text(entry.enabled ? L"启用" : L"已停用"); }
  bool statusLink() const {
    return !customView && IsWindowEnabled(control(registryLocation));
  }

  void rescale(UINT value) {
    Window::rescale(value);
    auto old = symbols;
    symbols = ui::makeSymbolFont(dpi, 16);
    auto oldHeading = headingFont;
    headingFont = ui::makeFont(dpi, 23, FW_SEMIBOLD);
    SendDlgItemMessageW(window, pageHeading, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    DeleteObject(oldHeading);
    if (tips) SendMessageW(tips, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    DeleteObject(old);
  }

  const wchar_t *glyph(int id) const {
    switch (id) {
    case extensionDrop: return L"\xE70D";
    case refresh: return L"\xE72C";
    case appearance: case edit: return L"\xE70F";
    case restore: return L"\xE7A7";
    case copyCommand: return L"\xE8C8";
    case registryLocation: return L"\xE943";
    case details: return L"\xE946";
    case add: return L"\xE710";
    case remove: return L"\xE74D";
    case toggle: {
      const auto row = selectedCommand();
      return row >= 0 && size_t(row) < displayed.size() && !custom[displayed[row]].enabled ? L"\xE768" : L"\xE769";
    }
    case up: return L"\xE74A";
    case down: return L"\xE74B";
    default: return L"";
    }
  }
  void drawActionIcon(HDC dc, int id, RECT rect, COLORREF color) const {
    if (id == visibility) {
      const auto *entry = original();
      rect.left += (rect.right - rect.left - px(16)) / 2;
      drawVisibility(dc, rect, entry && entry->hidden, color);
      return;
    }
    auto old = SelectObject(dc, symbols);
    ui::text(dc, glyph(id), rect, color, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
  }
  void drawAction(HWND button) const {
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(button, &paint);
    RECT rect{};
    GetClientRect(button, &rect);
    auto &p = theme.colors;
    FillRect(dc, &rect, theme.background);
    POINT cursor{};
    GetCursorPos(&cursor); ScreenToClient(button, &cursor);
    const bool enabled = IsWindowEnabled(button) != FALSE;
    const bool hovered = enabled && PtInRect(&rect, cursor);
    const bool pressed = enabled && (SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED);
    const bool focus = enabled && GetFocus() == button && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    const auto fill = pressed ? p.pressed : hovered ? p.hover : p.field;
    ui::rounded(dc, rect, fill, focus ? p.accent : p.border, px(5));
    drawActionIcon(dc, GetDlgCtrlID(button), rect,
        enabled ? p.highContrast && (pressed || hovered) ? p.onAccent : p.text : p.muted);
    EndPaint(button, &paint);
  }
  static LRESULT CALLBACK actionProc(HWND button, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto &state = *reinterpret_cast<Manager *>(data);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(button, actionProc, id);
    if (message == WM_PAINT) { state.drawAction(button); return 0; }
    if (message == WM_MOUSEMOVE) {
      TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, button, 0};
      TrackMouseEvent(&tracking);
    }
    if (message == WM_MOUSEMOVE || message == WM_MOUSELEAVE)
      InvalidateRect(button, nullptr, FALSE);
    return DefSubclassProc(button, message, w, l);
  }
  void drawStatus(HWND button) const {
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(button, &paint);
    RECT rect{};
    GetClientRect(button, &rect);
    FillRect(dc, &rect, theme.background);
    auto &p = theme.colors;
    const bool link = statusLink();
    const bool expanded = rect.bottom > px(34);
    RECT text = rect;
    InflateRect(&text, -px(7), 0);
    if (expanded) { text.top += px(4); text.bottom -= px(4); }
    auto old = SelectObject(dc, font);
    ui::text(dc, expanded ? statusText : windowText(button), text, link ? p.accent : p.muted,
        DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS | (expanded ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE | DT_VCENTER));
    SelectObject(dc, old);
    EndPaint(button, &paint);
  }
  static LRESULT CALLBACK statusProc(HWND button, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto &state = *reinterpret_cast<Manager *>(data);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(button, statusProc, id);
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_SETTEXT) {
      // A native button can paint immediately when its caption changes.
      SendMessageW(button, WM_SETREDRAW, FALSE, 0);
      auto result = DefSubclassProc(button, message, w, l);
      SendMessageW(button, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(button, nullptr, FALSE);
      return result;
    }
    if (message == WM_PAINT) { state.drawStatus(button); return 0; }
    if (message == WM_SETCURSOR && state.statusLink()) {
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    }
    return DefSubclassProc(button, message, w, l);
  }
  void prepareActions() {
    tips = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, window, nullptr, instance, nullptr);
    if (tips) {
      SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, px(320));
      SendMessageW(tips, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    auto addTip = [&](HWND child) {
      if (!tips) return;
      TOOLINFOW tool{sizeof(tool)};
      tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      tool.hwnd = window;
      tool.uId = reinterpret_cast<UINT_PTR>(child);
      tool.lpszText = LPSTR_TEXTCALLBACKW;
      SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    };
    constexpr std::array extraActions{int(refreshAction), int(extensionDrop)};
    for (const auto &actions : {std::span<const int>(existingActions), std::span<const int>(customActions),
                               std::span<const int>(extraActions)}) {
      for (int id : actions) {
        auto child = control(id);
        SetWindowSubclass(child, actionProc, 2, reinterpret_cast<DWORD_PTR>(this));
        addTip(child);
      }
    }
    auto status = control(explanation);
    SetWindowSubclass(status, statusProc, 2, reinterpret_cast<DWORD_PTR>(this));
    addTip(status);
  }
  void destroyTips() {
    if (tips) { DestroyWindow(tips); tips = nullptr; }
  }

  void clearIcons() {
    for (auto *icons : {&existingIcons, &customIcons}) {
      for (auto icon : *icons) if (icon) DestroyIcon(icon);
      icons->clear();
    }
  }
  HICON loadIcon(const std::wstring &location, const std::wstring &fallback) const {
    auto path = location.empty() ? fallback : location;
    if (path.empty()) return nullptr;
    std::wstring expanded(32768, L'\0');
    auto length = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (length && length <= expanded.size()) {
      expanded.resize(length - 1);
      path = std::move(expanded);
    }
    int resource = 0;
    if (!location.empty()) {
      if (path.starts_with(L"@")) path.erase(0, 1);
      resource = PathParseIconLocationW(path.data());
      path.resize(wcslen(path.c_str()));
      PathUnquoteSpacesW(path.data());
      path.resize(wcslen(path.c_str()));
      // Shell icon locations can name a Windows resource DLL without a path.
      // Resolve bare filenames from System32 without searching the working directory.
      if (!path.empty() && path.find_first_of(L"\\/:") == path.npos && path != L"." && path != L"..") {
        wchar_t directory[MAX_PATH]{};
        auto size = GetSystemDirectoryW(directory, ARRAYSIZE(directory));
        if (size && size < ARRAYSIZE(directory)) path = (std::filesystem::path(directory) / path).native();
      }
    }
    if (path.starts_with(L"\\\\") || !std::filesystem::path(path).is_absolute()) return nullptr;
    HICON icon = nullptr;
    ExtractIconExW(path.c_str(), resource, nullptr, &icon, 1);
    if (icon) return icon;
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
        SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) return info.hIcon;
    return nullptr;
  }
  void drawMenuItem(const DRAWITEMSTRUCT &item) {
    const bool objectList = item.CtlID == category;
    const bool originalList = item.CtlID == originals;
    const auto &icons = originalList ? existingIcons : customIcons;
    const auto &p = theme.colors;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    FillRect(item.hDC, &item.rcItem, theme.background);
    if (item.itemID == UINT(-1)) return;
    RECT row = item.rcItem;
    row.right -= px(11);
    InflateRect(&row, -px(3), -px(2));
    if (originalList && item.itemID < existing.size() && existing[item.itemID].label.empty()) {
      const bool child = item.itemID < childRows.size() && childRows[item.itemID];
      row.left += px(child ? 29 : 8);
      row.right -= px(8);
      row.top += (row.bottom - row.top) / 2;
      row.bottom = row.top + 1;
      SetDCBrushColor(item.hDC, p.border);
      FillRect(item.hDC, &row, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      return;
    }
    const auto selectedFill = objectList ? p.accent : p.hover;
    if (selected) ui::rounded(item.hDC, row, selectedFill, selectedFill, px(4));
    RECT text = row;
    text.left += px(objectList ? 9 : 28);
    text.right -= px(6);
    const bool child = originalList && item.itemID < childRows.size() && childRows[item.itemID];
    const int indent = child ? px(20) : 0;
    text.left += indent;
    if (!objectList && item.itemID < icons.size() && icons[item.itemID]) {
      const bool dim = originalList ? item.itemID < existing.size() && existing[item.itemID].hidden :
          item.itemID < displayed.size() && !custom[displayed[item.itemID]].enabled;
      const int x = item.rcItem.left + px(7) + indent;
      const int y = (item.rcItem.top + item.rcItem.bottom - px(16)) / 2;
      if (dim) {
        SetDCBrushColor(item.hDC, p.muted);
        DrawStateW(item.hDC, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)), nullptr,
                   reinterpret_cast<LPARAM>(icons[item.itemID]), 0, x, y, px(16), px(16), DST_ICON | DSS_MONO);
      }
      else DrawIconEx(item.hDC, x, y, icons[item.itemID], px(16), px(16), 0, nullptr, DI_NORMAL);
    }
    const auto ink = selected && (objectList || p.highContrast) ? p.onAccent : p.text;
    auto old = SelectObject(item.hDC, font);
    if (objectList && item.itemID < sceneLabels.size()) {
      ui::text(item.hDC, lang::text(sceneLabels[item.itemID]), text, ink);
    } else if (originalList && item.itemID < existing.size()) {
      RECT state = text;
      state.left = state.right - statusWidth();
      text.right = state.left - px(8);
      const auto &entry = existing[item.itemID];
      ui::text(item.hDC, entry.label, text, entry.hidden && !(selected && p.highContrast) ? p.muted : ink);
      if (item.itemID < originalRows.size() && submenuEnds.contains(originalRows[item.itemID])) {
        RECT arrow = state;
        arrow.left = arrow.right - px(18);
        ui::text(item.hDC, expandedMenus.contains(originalRows[item.itemID]) || !searchText.empty() ? L"▾" : L"▸", arrow, ink);
      }
      const auto color = selected && p.highContrast ? ink : p.muted;
      if (entry.canHide) drawVisibility(item.hDC, state, !entry.hidden, color);
    } else if (!objectList && !originalList && item.itemID < displayed.size()) {
      RECT state = text;
      state.left = state.right - statusWidth();
      text.right = state.left - px(8);
      const auto &entry = custom[displayed[item.itemID]];
      ui::text(item.hDC, entry.name, text, !entry.enabled && !(selected && p.highContrast) ? p.muted : ink);
      drawVisibility(item.hDC, state, entry.enabled, selected && p.highContrast ? ink : p.muted);
    }
    SelectObject(item.hDC, old);
    if ((item.itemState & ODS_FOCUS) && !(item.itemState & ODS_NOFOCUSRECT)) {
      if (objectList) {
        RECT marker{row.left + px(3), row.top + px(8), row.left + px(5), row.bottom - px(8)};
        ui::rounded(item.hDC, marker, ink, ink, px(1));
      }
    }
  }

  ui::ScrollBar &scroll(HWND field) {
    switch (GetDlgCtrlID(field)) {
    case category: return scrollbars[0];
    case originals: return scrollbars[1];
    default: return scrollbars[2];
    }
  }
  void syncScroll(HWND field) {
    auto &bar = scroll(field);
    if (!bar.window()) return;
    RECT rect{};
    GetClientRect(field, &rect);
    const auto total = static_cast<int>(SendMessageW(field, LB_GETCOUNT, 0, 0));
    const auto top = static_cast<int>(SendMessageW(field, LB_GETTOPINDEX, 0, 0));
    const int track = px(10);
    bar.move(std::max(0, int(rect.right) - track), 0, track, std::max(0, int(rect.bottom)));
    bar.configure(total, std::max(1, int(rect.bottom) / std::max(1, itemHeight(GetDlgCtrlID(field)))), top, dpi);
  }
  void prepareScroll(int id, int scrollId) {
    const auto field = control(id);
    scroll(field).create(field, scrollId, theme, [field](int top) {
      SendMessageW(field, LB_SETTOPINDEX, top, 0);
    });
    SetWindowSubclass(field, scrollProc, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  static LRESULT CALLBACK scrollProc(HWND field, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto &state = *reinterpret_cast<Manager *>(data);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(field, scrollProc, id);
    if (message == WM_MOUSEWHEEL) { state.scroll(field).wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0; }
    if (message == WM_VSCROLL && reinterpret_cast<HWND>(l) == state.scroll(field).window()) {
      state.scroll(field).command(w); return 0;
    }
    if (message == WM_CONTEXTMENU && (GetDlgCtrlID(field) == originals || GetDlgCtrlID(field) == commands)) {
      state.itemMenu(field, {GET_X_LPARAM(l), GET_Y_LPARAM(l)}); return 0;
    }
    if (message == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
      POINT point{};
      GetCursorPos(&point); ScreenToClient(field, &point);
      if (state.visibilityAction(field, point)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE;
      }
    }
    const int action = message == WM_LBUTTONUP
        ? state.visibilityAction(field, {GET_X_LPARAM(l), GET_Y_LPARAM(l)}) : 0;
    auto result = DefSubclassProc(field, message, w, l);
    if (GetDlgCtrlID(field) == originals && state.searchText.empty() &&
        (message == WM_LBUTTONUP || (message == WM_KEYDOWN && (w == VK_LEFT || w == VK_RIGHT)))) {
      const auto row = SendMessageW(field, LB_GETCURSEL, 0, 0);
      const bool keyboard = message == WM_KEYDOWN && (w == VK_LEFT || w == VK_RIGHT);
      RECT item{};
      const bool arrowClick = message == WM_LBUTTONUP && !action && row >= 0 &&
          SendMessageW(field, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item)) != LB_ERR &&
          GET_X_LPARAM(l) >= item.right - state.px(39) && GET_X_LPARAM(l) < item.right - state.px(11) &&
          GET_Y_LPARAM(l) >= item.top && GET_Y_LPARAM(l) < item.bottom;
      if ((keyboard || arrowClick) && row >= 0 && size_t(row) < state.originalRows.size()) {
        const auto key = state.originalRows[row];
        if (state.submenuEnds.contains(key)) {
          const bool open = keyboard ? w == VK_RIGHT : !state.expandedMenus.contains(key);
          if (open) state.expandedMenus.insert(key);
          else state.expandedMenus.erase(key);
          state.filter(static_cast<int>(row), state.selectedCommand());
          return 0;
        }
      }
    }
    if (action) {
      const auto hit = SendMessageW(field, LB_ITEMFROMPOINT, 0, l);
      if (!HIWORD(hit) && SendMessageW(field, LB_GETCURSEL, 0, 0) == LOWORD(hit))
        SendMessageW(state.window, WM_COMMAND, MAKEWPARAM(action, BN_CLICKED), directEyeClick);
    }
    switch (message) {
    case WM_SIZE: case WM_KEYDOWN: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case LB_SETCURSEL: case LB_SETTOPINDEX: case LB_RESETCONTENT:
      state.syncScroll(field); break;
    }
    return result;
  }
  struct PopupRow {
    MSAAMENUINFO accessible{};
    std::wstring caption;
    int action = 0;
    bool startsGroup = false;
  };
  void measurePopupRow(MEASUREITEMSTRUCT &item) const {
    const auto &row = *reinterpret_cast<const PopupRow *>(item.itemData);
    SIZE label{};
    auto dc = GetDC(window);
    if (dc) {
      auto old = SelectObject(dc, font);
      GetTextExtentPoint32W(dc, row.caption.c_str(), static_cast<int>(row.caption.size()), &label);
      SelectObject(dc, old);
      ReleaseDC(window, dc);
    }
    item.itemWidth = std::clamp<int>(label.cx + px(58), px(174), px(360));
    item.itemHeight = px(row.startsGroup ? 40 : 32);
  }
  void drawPopupRow(const DRAWITEMSTRUCT &item) const {
    const auto &entry = *reinterpret_cast<const PopupRow *>(item.itemData);
    const auto &colors = theme.colors;
    auto row = item.rcItem;
    FillRect(item.hDC, &row, theme.card);
    if (entry.startsGroup) {
      RECT divider{row.left + px(10), row.top + px(3), row.right - px(10), row.top + px(4)};
      ui::rounded(item.hDC, divider, colors.border, colors.border, 0);
      row.top += px(8);
    }
    const bool enabled = !(item.itemState & (ODS_DISABLED | ODS_GRAYED));
    const bool selected = (item.itemState & ODS_SELECTED) && enabled;
    InflateRect(&row, -px(4), -px(2));
    if (selected) ui::rounded(item.hDC, row, colors.hover, colors.hover, px(5));
    const auto ink = !enabled ? colors.muted : selected && colors.highContrast ? colors.onAccent : colors.text;
    RECT icon{row.left + px(8), row.top, row.left + px(28), row.bottom};
    drawActionIcon(item.hDC, entry.action, icon, ink);
    row.left += px(36);
    row.right -= px(10);
    auto old = SelectObject(item.hDC, font);
    ui::text(item.hDC, entry.caption, row, ink);
    SelectObject(item.hDC, old);
  }
  void itemMenu(HWND list, POINT point) {
    const bool keyboard = point.x == -1 && point.y == -1;
    int row = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (!keyboard) {
      auto local = point;
      ScreenToClient(list, &local);
      auto hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(local.x, local.y));
      if (HIWORD(hit)) return;
      row = LOWORD(hit);
      RECT item{};
      if (SendMessageW(list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item)) == LB_ERR ||
          !PtInRect(&item, local)) return;
      SendMessageW(list, LB_SETCURSEL, row, 0);
    }
    if (row < 0) return;
    if (keyboard) {
      RECT item{};
      if (SendMessageW(list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&item)) == LB_ERR) return;
      point = {item.left + px(16), item.bottom};
      ClientToScreen(list, &point);
    }
    SetFocus(list);
    updateActions();
    struct Popup {
      HMENU handle = CreatePopupMenu();
      ~Popup() { if (handle) DestroyMenu(handle); }
    } menu;
    if (!menu.handle) return;
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = theme.card;
    SetMenuInfo(menu.handle, &info);
    const std::array<int, 5> customPopupActions{edit, toggle, up, down, remove};
    const bool existingView = list == control(originals);
    std::span<const int> actions = existingView ? std::span<const int>(existingActions) : std::span<const int>(customPopupActions);
    std::array<PopupRow, existingActions.size()> entries;
    for (UINT index = 0; index < actions.size(); ++index) {
      const int action = actions[index];
      auto button = control(action);
      auto &entry = entries[index];
      entry.caption = windowText(button);
      entry.action = action;
      entry.startsGroup = action == registryLocation || action == remove;
      entry.accessible = {MSAA_MENU_SIG, static_cast<DWORD>(entry.caption.size()), entry.caption.data()};
      MENUITEMINFOW item{sizeof(item)};
      item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_STRING | MIIM_DATA;
      item.fType = MFT_OWNERDRAW;
      item.wID = action;
      item.fState = IsWindowEnabled(button) ? MFS_ENABLED : MFS_DISABLED;
      item.dwTypeData = entry.caption.data();
      item.dwItemData = reinterpret_cast<ULONG_PTR>(&entry);
      if (!InsertMenuItemW(menu.handle, index, TRUE, &item)) return;
    }
    const auto action = TrackPopupMenu(menu.handle, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
        point.x, point.y, 0, window, nullptr);
    if (action) SendMessageW(window, WM_COMMAND, MAKEWPARAM(action, BN_CLICKED), 0);
  }

  void createControls() {
    auto heading = create(pageHeading, L"STATIC", title, SS_NOPREFIX | SS_ENDELLIPSIS);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    button(enable, L"启用右键菜单管理", BS_AUTOCHECKBOX);
    ui::toggleSwitch(control(enable));
    constexpr auto listStyle = WS_TABSTOP | WS_CLIPCHILDREN | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS;
    create(category, L"LISTBOX", L"", listStyle | LBS_OWNERDRAWFIXED);
    for (auto label : sceneLabels) append(control(category), lang::text(label));
    SendDlgItemMessageW(window, category, LB_SETCURSEL, 0, 0);
    create(extension, L"EDIT", L".txt", WS_TABSTOP | ES_AUTOHSCROLL);
    button(extensionDrop, L"选择扩展名");
    SendDlgItemMessageW(window, extension, EM_SETLIMITTEXT, 40, 0);
    ui::surface(control(extension));
    SendDlgItemMessageW(window, extension, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(3), px(3)));
    button(refresh, L"刷新");
    button(restoreAll, L"批量还原");
    button(originalLabel, L"已有菜单项", BS_DEFPUSHBUTTON);
    button(commandLabel, L"自定义命令");
    create(originals, L"LISTBOX", L"", listStyle | LBS_OWNERDRAWVARIABLE);
    create(commands, L"LISTBOX", L"", listStyle | LBS_OWNERDRAWFIXED);
    for (int id : {category, originals, commands}) quick_find::attach(control(id), {}, [this, id](int row) {
      const auto &icons = id == originals ? existingIcons : customIcons;
      return id != category && row >= 0 && size_t(row) < icons.size() ?
          quick_find::CandidateIcon::copy(icons[row]) : quick_find::CandidateIcon{};
    });
    label(nameColumn, L"名称"); label(statusColumn, L"状态");
    ui::muted(control(nameColumn)); ui::muted(control(statusColumn));
    create(menuSearch, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL);
    SendDlgItemMessageW(window, menuSearch, EM_SETLIMITTEXT, 160, 0);
    SendDlgItemMessageW(window, menuSearch, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(lang::text(L"搜索菜单")));
    prepareScroll(category, categoryScroll);
    prepareScroll(originals, originalsScroll);
    prepareScroll(commands, commandsScroll);
    button(visibility, L"隐藏");
    button(appearance, L"名称和图标…");
    button(restore, L"还原");
    button(copyCommand, L"加入自定义");
    button(add, L"添加…"); button(edit, L"编辑…"); button(remove, L"删除");
    button(toggle, L"停用"); button(up, L"上移"); button(down, L"下移");
    button(registryLocation, L"打开注册表…");
    button(details, L"查看详情");
    button(explanation, L"");
  }
  int categoryLimit(int width) const { return std::max(116, MulDiv(width, 96, dpi) - 486); }
  int actionLimit(int width) const { return std::clamp(MulDiv(width, 96, dpi) - 590, 32, 96); }
  int statusLimit(int height) const { return std::clamp(MulDiv(height, 96, dpi) - 255, 23, 180); }
  Divider dividerAt(POINT point) const {
    if (PtInRect(&categoryDivider, point)) return Divider::Category;
    if (PtInRect(&actionsDivider, point)) return Divider::Actions;
    if (PtInRect(&statusDivider, point)) return Divider::Status;
    return Divider::None;
  }
  static bool dividerCursor(Divider divider) {
    if (divider == Divider::None) return false;
    SetCursor(LoadCursorW(nullptr, divider == Divider::Status ? IDC_SIZENS : IDC_SIZEWE));
    return true;
  }
  void resizeDivider(POINT point) {
    RECT rect{};
    GetClientRect(window, &rect);
    switch (dragging) {
    case Divider::Category:
      categoryWidth = std::clamp(MulDiv(point.x - px(16), 96, dpi) - 8, 116, categoryLimit(rect.right));
      break;
    case Divider::Actions:
      actionsWidth = std::clamp(MulDiv(rect.right - point.x, 96, dpi) - 20, 32, actionLimit(rect.right));
      break;
    case Divider::Status:
      statusHeight = std::clamp(MulDiv(rect.bottom - point.y, 96, dpi) - 20, 23, statusLimit(rect.bottom));
      break;
    case Divider::None: return;
    }
    layout();
  }
  void paintDividers() const {
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(window, &paint);
    const auto hot = dragging != Divider::None ? dragging : hoveredDivider;
    auto paintLine = [&](RECT bounds, Divider divider) {
      if (IsRectEmpty(&bounds)) return;
      if (divider == Divider::Status) {
        bounds.top += (bounds.bottom - bounds.top) / 2;
        bounds.bottom = bounds.top + px(1);
      } else {
        bounds.left += (bounds.right - bounds.left) / 2;
        bounds.right = bounds.left + px(1);
      }
      auto brush = CreateSolidBrush(hot == divider ? theme.colors.accent : theme.colors.border);
      FillRect(dc, &bounds, brush);
      DeleteObject(brush);
    };
    paintLine(categoryDivider, Divider::Category);
    paintLine(actionsDivider, Divider::Actions);
    paintLine(statusDivider, Divider::Status);
    EndPaint(window, &paint);
  }
  void layout() {
    RECT client{}; GetClientRect(window, &client);
    const int edge = px(16), gap = px(8), height = px(30);
    const int leftWidth = px(std::min(categoryWidth, categoryLimit(client.right)));
    const int content = edge + leftWidth + px(16);
    const int right = client.right - edge, width = right - content;
    auto dc = GetDC(window);
    const auto measure = [&](int id, HFONT face) {
      if (!dc) return 0;
      const auto caption = windowText(control(id));
      SIZE size{};
      auto old = SelectObject(dc, face);
      GetTextExtentPoint32W(dc, caption.c_str(), static_cast<int>(caption.size()), &size);
      SelectObject(dc, old);
      return static_cast<int>(size.cx);
    };
    const int tabTextWidth = std::max(measure(originalLabel, font), measure(commandLabel, font));
    const int extensionSpace = extensionScene() ? px(140) : 0;
    const int tabLimit = std::max(px(66), (width - px(108) - gap * 2 - extensionSpace) / 2);
    const int buttonWidth = std::min(std::max(px(112), tabTextWidth + px(28)), tabLimit);
    const int headingTextWidth = measure(pageHeading, headingFont);
    const int enableWidth = std::min(px(226), measure(enable, font) + px(48));
    if (dc) ReleaseDC(window, dc);
    const int header = px(26), tabs = px(76);
    const int panelHeight = px(std::min(statusHeight, statusLimit(client.bottom)));
    const int statusBar = client.bottom - edge - panelHeight;
    const int railWidth = px(std::min(actionsWidth, actionLimit(client.right)));
    const int actions = right - railWidth;
    const int listRight = actions - gap;
    const int listWidth = listRight - content;
    const int searchTop = tabs;
    const int secondary = tabs + height + gap;
    const int columns = secondary;
    const int listY = columns + px(21);
    auto buttonRow = [&](const auto &ids, int top, int width) {
      int left = content;
      for (auto id : ids) { bounds(id, left, top, width, height); left += width + gap; }
    };
    const int restoreWidth = px(132);
    const int enableLeft = right - enableWidth;
    const int restoreLeft = enableLeft - gap - restoreWidth;
    const int refreshLeft = restoreLeft - gap - px(32);
    const int headingWidth = std::min(std::max(px(188), headingTextWidth + px(4)),
                                      std::max(px(100), (refreshLeft - edge - gap) / 2));
    bounds(pageHeading, edge, header + px(4), headingWidth, height);
    bounds(enable, enableLeft, header, enableWidth, height);
    bounds(refresh, refreshLeft, header, px(32), height);
    bounds(restoreAll, restoreLeft, header, restoreWidth, height);
    bounds(category, edge, tabs, leftWidth, statusBar - gap - tabs);
    buttonRow(std::array{originalLabel, commandLabel}, tabs, buttonWidth);
    bounds(nameColumn, content + px(31), columns, listWidth - statusWidth() - px(55), px(20));
    const int searchLeft = content + 2 * (buttonWidth + gap);
    const int searchWidth = std::min(px(150), std::max(px(48), right - searchLeft - extensionSpace));
    bounds(menuSearch, searchLeft + px(27), searchTop + px(4), searchWidth - px(33), px(19));
    const int extensionInput = searchLeft + searchWidth + gap + px(6);
    const int extensionWidth = std::min(px(100), std::max(0, right - extensionInput - px(32)));
    bounds(extension, extensionInput, tabs + px(5), extensionWidth, height - px(10));
    bounds(extensionDrop, extensionInput + extensionWidth + px(4), tabs, px(28), height);
    bounds(statusColumn, listRight - statusWidth() - px(20), columns, statusWidth(), px(20));
    bounds(originals, content, listY, listWidth, statusBar - gap - listY);
    bounds(commands, content, listY, listWidth, statusBar - gap - listY);
    const int actionHeight = std::clamp((statusBar - gap - listY) / 6, px(20), height);
    const int actionGap = std::clamp((statusBar - gap - listY - 6 * actionHeight) / 5, 0, gap);
    for (const auto &group : {existingActions, customActions}) {
      int top = listY;
      for (int id : group) {
        bounds(id, actions, top, railWidth, actionHeight);
        top += actionHeight + actionGap;
      }
    }
    bounds(explanation, edge, statusBar, right - edge, panelHeight);
    categoryDivider = {edge + leftWidth, columns, content, statusBar - gap};
    actionsDivider = {listRight, listY, actions, statusBar - gap};
    statusDivider = {edge, statusBar - gap, right, statusBar};
    for (int id : {category, originals, commands}) {
      if (id == originals) {
        for (size_t i = 0; i < existing.size(); ++i)
          SendDlgItemMessageW(window, id, LB_SETITEMHEIGHT, i, existing[i].label.empty() ? px(7) : itemHeight(id));
      } else SendDlgItemMessageW(window, id, LB_SETITEMHEIGHT, 0, itemHeight(id));
      syncScroll(control(id));
    }
    InvalidateRect(window, nullptr, TRUE);
  }
  bool customScene() const {
    return scene != Scene::New && scene != Scene::SendTo && scene != Scene::OpenWith;
  }
  bool extensionScene() const { return scene == Scene::Extension || scene == Scene::OpenWith; }
  bool sameScene(const Command &entry) const {
    return entry.scene == scene && (scene != Scene::Extension ||
        CompareStringOrdinal(entry.extension.c_str(), -1, currentExtension.c_str(), -1, TRUE) == CSTR_EQUAL);
  }
  int selectedOriginal() const { return index(window, originals); }
  int selectedCommand() const { return index(window, commands); }
  const ExistingItem *original() const {
    auto row = selectedOriginal();
    return row >= 0 && size_t(row) < existing.size() && !existing[row].label.empty() ? &existing[row] : nullptr;
  }
  void updateView() {
    if (!customScene()) customView = false;
    EnableWindow(control(commandLabel), customScene());
    SendDlgItemMessageW(window, originalLabel, BM_SETSTYLE, customView ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON, TRUE);
    SendDlgItemMessageW(window, commandLabel, BM_SETSTYLE, customView ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    ShowWindow(control(originals), customView ? SW_HIDE : SW_SHOWNA);
    ShowWindow(control(commands), customView ? SW_SHOWNA : SW_HIDE);
  }
  void updateActions() {
    const auto *entry = original();
    const auto row = selectedCommand();
    const bool hasCommand = row >= 0 && size_t(row) < displayed.size();
    if (tips) SendMessageW(tips, TTM_POP, 0, 0);
    set(visibility, lang::text(entry && entry->hidden ? L"恢复显示" : L"隐藏"));
    enableControl(visibility, entry && entry->canHide);
    enableControl(appearance, active && entry && entry->canEdit);
    enableControl(restore, entry && entry->managed);
    enableControl(copyCommand, entry && !entry->program.empty() && customScene());
    enableControl(add, customScene());
    enableControl(registryLocation, entry && (entry->source.starts_with(L"HKCU\\") || entry->source.starts_with(L"HKLM\\")));
    enableControl(details, entry != nullptr);
    const bool selectedEntry = entry && !customView;
    auto summary = selectedEntry ? entry->source : std::wstring(lang::text(
        customScene() ? L"自定义命令按列表顺序显示在 ShibaKit 子菜单。" : L"此类别可查看已有菜单项。"));
    const bool blockedExtension = selectedEntry && entry->shellBlock && !entry->handlerRegistration &&
        entry->hidden && entry->managed;
    if (blockedExtension)
      summary = lang::text(L"已设为隐藏。若右键菜单仍显示，请在任务管理器中重启 Windows 资源管理器。");
    else if (selectedEntry && !entry->canHide && summary.empty())
      summary = lang::text(L"此菜单项由资源管理器生成，当前仅可查看。");
    statusText = summary;
    if (blockedExtension && !entry->source.empty()) statusText += L"\r\n" + entry->source;
    if (selectedEntry && !entry->command.empty()) statusText += L"\r\n" + entry->command;
    set(explanation, summary.c_str());
    auto status = control(explanation);
    auto style = GetWindowLongPtrW(status, GWL_STYLE);
    const auto updatedStyle = statusLink() ? style | WS_TABSTOP : style & ~WS_TABSTOP;
    if (style != updatedStyle) SetWindowLongPtrW(status, GWL_STYLE, updatedStyle);
    InvalidateRect(status, nullptr, FALSE);
    for (int id : {edit, remove, toggle}) enableControl(id, hasCommand);
    enableControl(up, searchText.empty() && hasCommand && row > 0);
    enableControl(down, searchText.empty() && hasCommand && size_t(row + 1) < displayed.size());
    set(toggle, lang::text(hasCommand && !custom[displayed[row]].enabled ? L"启用" : L"停用"));
    for (int id : existingActions)
      ShowWindow(control(id), !customView && IsWindowEnabled(control(id)) ? SW_SHOWNA : SW_HIDE);
    for (int id : customActions)
      ShowWindow(control(id), customView && IsWindowEnabled(control(id)) ? SW_SHOWNA : SW_HIDE);
  }
  void reload(int originalRow = -1, int customRow = -1) {
    pendingOriginal = originalRow;
    pendingCustom = customRow;
    if (loading.valid()) { reloadPending = true; return; }
    const int selection = std::clamp(index(window, category), 0, int(scenes.size() - 1));
    scene = scenes[selection];
    currentExtension = suffix(window);
    const auto selectedScene = scene;
    const auto extension = currentExtension;
    auto stage = std::make_shared<std::atomic<bool>>(false);
    std::packaged_task<LoadResult()> task([selectedScene, extension, stage] {
      const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      struct Apartment { HRESULT result; ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); } } apartment{com};
      return loadData(selectedScene, extension, SUCCEEDED(com), *stage);
    });
    if (!SetTimer(window, loadTimer, 80, nullptr)) fn::fail(L"无法启动菜单加载");
    try {
      auto result = task.get_future();
      std::thread(std::move(task)).detach();
      loading = std::move(result);
      readingShell = std::move(stage);
    } catch (...) { KillTimer(window, loadTimer); throw; }
    statusText = lang::text(L"正在读取右键菜单…");
    set(explanation, statusText.c_str());
    EnableWindow(control(originals), FALSE);
    EnableWindow(control(commands), FALSE);
  }
  static LoadResult loadData(Scene scene, const std::wstring &extension, bool preview, std::atomic<bool> &readingShell) {
    LoadResult result;
    result.registered = context::inventory(scene, extension);
    result.enabled = context::commandsEnabled();
    result.commands = context::loadCommands();
    if (scene == Scene::Folder) {
      for (int op = 0; op < 3; ++op) {
        ExistingItem item;
        item.label = lang::text(fn::titles[op]);
        item.noteOperation = op;
        item.canHide = true;
        item.hidden = !navigation::notesEnabled() || navigation::noteCommandHidden(op);
        result.registered.push_back(std::move(item));
      }
    }
    std::vector<std::wstring> handlerCandidates, hiddenHandlers;
    for (const auto &entry : result.registered) {
      if (!entry.shellBlock) continue;
      auto &candidates = entry.hidden ? hiddenHandlers : handlerCandidates;
      if ((!entry.hidden || (entry.managed && entry.canHide)) &&
          std::find(candidates.begin(), candidates.end(), entry.shellBlockId) == candidates.end())
        candidates.push_back(entry.shellBlockId);
    }
    std::vector<context::MenuPreviewItem> hiddenItems;
    if (preview) {
      readingShell = true;
      result.order = readActualOrder(scene, extension, handlerCandidates, hiddenHandlers, hiddenItems);
    }
    result.hidden = std::move(hiddenItems);
    return result;
  }
  void finishLoad() {
    if (!loading.valid()) return;
    if (loading.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      const auto stage = lang::text(L"正在读取资源管理器中的实际菜单…");
      if (readingShell && *readingShell && statusText != stage) {
        statusText = stage;
        set(explanation, stage);
      }
      return;
    }
    KillTimer(window, loadTimer);
    auto result = std::move(loading);
    if (std::exchange(reloadPending, false)) { reload(pendingOriginal, pendingCustom); return; }
    EnableWindow(control(originals), TRUE);
    EnableWindow(control(commands), TRUE);
    LoadResult data;
    try { data = result.get(); }
    catch (...) {
      inventoryItems.clear();
      inventoryIcons.clear();
      submenuEnds.clear();
      expandedMenus.clear();
      filter();
      statusText = lang::text(L"右键菜单加载失败，请刷新重试。");
      set(explanation, statusText.c_str());
      throw;
    }
    active = data.enabled;
    auto &registered = data.registered;
    const auto &shellOrder = data.order;
    const auto &hiddenItems = data.hidden;
    const int originalRow = pendingOriginal, customRow = pendingCustom;
    submenuEnds.clear();
    expandedMenus.clear();
    inventoryIcons.clear();
    inventoryItems.clear();
    std::vector<bool> matched(registered.size());
    auto sameName = [](const std::wstring &a, const std::wstring &b) {
      return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    };
    auto addRow = [&](ExistingItem item, const context::MenuPreviewItem *preview) {
      const auto parent = inventoryItems.size();
      auto children = std::move(item.children);
      inventoryItems.push_back(std::move(item));
      inventoryIcons.push_back(preview ? preview->icon : context::MenuIcon{});
      if (preview && !preview->children.empty()) {
        for (const auto &child : preview->children) {
          ExistingItem row;
          if (!child.separator) {
            const auto found = std::find_if(children.begin(), children.end(), [&](const auto &candidate) {
              return sameName(candidate.label, child.caption);
            });
            if (found != children.end()) row = std::move(*found);
            row.label = child.caption;
            row.hidden = row.hidden || inventoryItems[parent].hidden;
          }
          inventoryItems.push_back(std::move(row));
          inventoryIcons.push_back(child.icon);
        }
      } else {
        for (auto &child : children) {
          inventoryItems.push_back(std::move(child));
          inventoryIcons.emplace_back();
        }
      }
      if (parent + 1 < inventoryItems.size()) submenuEnds.emplace(parent, inventoryItems.size());
    };
    for (const auto &preview : shellOrder) {
      ExistingItem item;
      if (!preview.separator) {
        for (size_t i = 0; i < registered.size(); ++i) {
          const auto &candidate = registered[i];
          if (candidate.hidden) continue;
          const bool handlerMatch = candidate.shellBlock && !preview.handlerClsid.empty() &&
              sameName(candidate.shellBlockId, preview.handlerClsid);
          const bool verbMatch = preview.handlerClsid.empty() && !candidate.shellBlock &&
              !matched[i] && sameName(candidate.label, preview.caption);
          if (!handlerMatch && !verbMatch) continue;
          matched[i] = true;
          item = candidate;
          break;
        }
        item.label = preview.caption;
      }
      addRow(std::move(item), &preview);
    }
    for (size_t i = 0; i < registered.size(); ++i) {
      if (matched[i]) continue;
      auto &entry = registered[i];
      bool hasPreview = false;
      if (entry.shellBlock && entry.hidden && entry.managed) {
        for (const auto &preview : hiddenItems) {
          if (!sameName(entry.shellBlockId, preview.handlerClsid)) continue;
          auto item = entry;
          item.label = preview.caption;
          addRow(std::move(item), &preview);
          hasPreview = true;
        }
      }
      if (!hasPreview) addRow(std::move(entry), nullptr);
    }
    custom = std::move(data.commands);
    SendDlgItemMessageW(window, enable, BM_SETCHECK, active ? BST_CHECKED : BST_UNCHECKED, 0);
    ShowWindow(control(extension), extensionScene() ? SW_SHOW : SW_HIDE);
    ShowWindow(control(extensionDrop), extensionScene() ? SW_SHOW : SW_HIDE);
    layout();
    InvalidateRect(window, nullptr, TRUE);
    filter(originalRow, customRow);
  }
  static std::vector<context::MenuPreviewItem> readActualOrder(Scene scene, const std::wstring &currentExtension,
                                                       std::span<const std::wstring> handlerCandidates,
                                                       std::span<const std::wstring> hiddenHandlers,
                                                       std::vector<context::MenuPreviewItem> &hiddenItems) {
    auto read = [&](const std::wstring &path, context::PreviewTarget target = context::PreviewTarget::Object) {
      return context::actualMenuOrder(nullptr, path, target, handlerCandidates, hiddenHandlers, &hiddenItems);
    };
    auto submenu = [](std::vector<context::MenuPreviewItem> items, std::wstring_view chinese, std::wstring_view english) {
      for (auto &item : items) {
        std::wstring_view caption = item.caption;
        if (caption.size() >= 3 && caption[caption.size() - 3] == L'(' && caption.back() == L')')
          caption.remove_suffix(3);
        if (!item.children.empty() && (caption == chinese || caption == english)) return std::move(item.children);
      }
      return std::vector<context::MenuPreviewItem>{};
    };
    if (scene == Scene::New)
      return submenu(context::actualMenuOrder(nullptr, L"", context::PreviewTarget::Background), L"新建", L"New");
    if (scene == Scene::Desktop)
      return read(L"", context::PreviewTarget::Background);
    std::error_code error;
    const auto temp = std::filesystem::temp_directory_path(error);
    if (error) return {};
    if (scene == Scene::Folder || scene == Scene::Drive || scene == Scene::FolderBackground)
      return read((scene == Scene::Drive ? temp.root_path() : temp).native(),
                  scene == Scene::FolderBackground ? context::PreviewTarget::Background : context::PreviewTarget::Object);
    if (scene == Scene::File) {
      wchar_t system[MAX_PATH]{};
      const auto length = GetSystemDirectoryW(system, ARRAYSIZE(system));
      if (!length || length >= ARRAYSIZE(system)) return {};
      return read((std::filesystem::path(system) / L"notepad.exe").native());
    }
    const auto sampleExtension = scene == Scene::SendTo ? std::wstring(L".png") : currentExtension;
    if (sampleExtension.size() < 2 || sampleExtension.size() > 32 ||
        sampleExtension.substr(1).find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789_-") != std::wstring::npos)
      return {};
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) return {};
    wchar_t name[40]{};
    if (!StringFromGUID2(id, name, ARRAYSIZE(name))) return {};
    const auto path = temp / (std::wstring(L"ShibaKit-") + name + sampleExtension);
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    CloseHandle(file);
    struct Sample { const std::filesystem::path &path; ~Sample() { DeleteFileW(path.c_str()); } } sample{path};
    auto items = read(path.native());
    if (scene == Scene::SendTo) return submenu(std::move(items), L"发送到", L"Send to");
    if (scene == Scene::OpenWith) return submenu(std::move(items), L"打开方式", L"Open with");
    return items;
  }
  void filter(int originalRow = -1, int customRow = -1) {
    searchText = windowText(control(menuSearch));
    auto matches = [&](const std::wstring &name) {
      return quick_find::matches(name, searchText);
    };
    SendDlgItemMessageW(window, originals, LB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(window, commands, LB_RESETCONTENT, 0, 0);
    clearIcons();
    existing.clear();
    originalRows.clear();
    childRows.clear();
    displayed.clear();
    size_t childEnd = 0;
    for (size_t i = 0; i < inventoryItems.size(); ++i) {
      const bool child = i < childEnd;
      const auto submenu = submenuEnds.find(i);
      if (submenu != submenuEnds.end()) childEnd = submenu->second;
      const auto &entry = inventoryItems[i];
      if (!matches(entry.label) && (entry.registeredLabel.empty() || !matches(entry.registeredLabel))) continue;
      existing.push_back(entry);
      originalRows.push_back(i);
      childRows.push_back(child);
      auto icon = i < inventoryIcons.size() && inventoryIcons[i] ? CopyIcon(inventoryIcons[i].get()) : nullptr;
      if (!icon) icon = loadIcon(entry.icon, entry.sendTo ? entry.source : L"");
      if (!icon && scene == Scene::Folder && entry.source.empty() &&
          std::any_of(std::begin(fn::titles), std::end(fn::titles), [&](const auto *caption) {
            return entry.label == lang::text(caption);
          })) {
        icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_SHIBAKIT), IMAGE_ICON,
                                            px(16), px(16), 0));
      }
      existingIcons.push_back(icon);
      append(control(originals), entry.label.empty() ? L"" : lang::format(L"{0} · {1}", {entry.label, status(entry)}));
      SendDlgItemMessageW(window, originals, LB_SETITEMHEIGHT, existing.size() - 1,
                          entry.label.empty() ? px(7) : itemHeight(originals));
      if (submenu != submenuEnds.end() && searchText.empty() && !expandedMenus.contains(i)) i = submenu->second - 1;
    }
    for (size_t i = 0; i < custom.size(); ++i) if (sameScene(custom[i]) && matches(custom[i].name)) {
      displayed.push_back(i);
      customIcons.push_back(loadIcon(custom[i].icon, custom[i].program));
      append(control(commands), lang::format(L"{0} · {1}", {custom[i].name, status(custom[i])}));
    }
    SendDlgItemMessageW(window, originals, LB_SETCURSEL,
        existing.empty() ? -1 : std::clamp(originalRow, 0, int(existing.size() - 1)), 0);
    SendDlgItemMessageW(window, commands, LB_SETCURSEL,
        displayed.empty() ? -1 : std::clamp(customRow, 0, int(displayed.size() - 1)), 0);
    updateView();
    updateActions();
    for (int id : {category, originals, commands}) syncScroll(control(id));
  }
  void persist(bool enabled, const std::vector<Command> &entries) {
    context::saveCommands(enabled, entries, quick::executable());
  }
  void restoreMany() {
    const auto conflicts = context::restoreAll();
    if (!conflicts) return;
    const auto message = lang::format(L"有 {0} 项未还原，可能已被其他程序修改。", {std::to_wstring(conflicts)});
    ui::message(window, message.c_str(), lang::text(title), MB_OK | MB_ICONWARNING);
  }
  void handle(int id, int code, bool direct) {
    auto before = selectedOriginal();
    auto row = selectedCommand();
    const auto *entry = original();
    const bool customSelected = row >= 0 && size_t(row) < displayed.size();
    if ((id == originals || id == commands) && code == LBN_SELCHANGE) { updateActions(); return; }
    if (id == category && code == LBN_SELCHANGE) { reload(); return; }
    if (loading.valid() && id != extension && id != refresh) return;
    if (id == menuSearch && code == EN_CHANGE) { filter(); return; }
    if (id == extension && code == EN_KILLFOCUS && extensionScene() && currentExtension != suffix(window)) {
      reload(); return;
    }
    if (code != BN_CLICKED && !(id == commands && code == LBN_DBLCLK)) return;
    if (id == extensionDrop) {
      ExtensionPicker picker(instance);
      picker.letter = extensionLetter;
      HKEY classes{};
      if (RegOpenKeyExW(HKEY_CLASSES_ROOT, nullptr, 0, KEY_ENUMERATE_SUB_KEYS, &classes) == ERROR_SUCCESS) {
        for (DWORD index = 0;; ++index) {
          wchar_t key[256]{};
          DWORD length = ARRAYSIZE(key);
          const auto result = RegEnumKeyExW(classes, index, key, &length, nullptr, nullptr, nullptr, nullptr);
          if (result == ERROR_NO_MORE_ITEMS) break;
          if (result == ERROR_MORE_DATA) continue;
          if (result != ERROR_SUCCESS) break;
          if (length > 1 && length <= 32 && key[0] == L'.' && iswalpha(key[1]) &&
              std::wstring_view(key + 1, length - 1).find_first_not_of(
                  L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::wstring_view::npos)
            picker.extensions[towupper(key[1]) - L'A'].emplace_back(key, length);
        }
        RegCloseKey(classes);
      }
      for (auto &group : picker.extensions) std::sort(group.begin(), group.end(), [](const auto &a, const auto &b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
      });
      WNDCLASSW cls{};
      cls.hInstance = instance; cls.lpfnWndProc = ExtensionPicker::proc;
      cls.lpszClassName = L"ShibaKit.ContextMenu.Extensions"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      RegisterClassW(&cls);
      RECT anchor{};
      GetWindowRect(control(extension), &anchor);
      MONITORINFO monitor{sizeof(monitor)};
      GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
      const int width = px(640), height = px(328);
      auto popup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, cls.lpszClassName, L"",
          WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
          std::clamp<int>(anchor.left, monitor.rcWork.left, std::max<int>(monitor.rcWork.left, monitor.rcWork.right - width)),
          anchor.bottom + height <= monitor.rcWork.bottom ? anchor.bottom : std::max<int>(monitor.rcWork.top, anchor.top - height),
          width, height, GetAncestor(window, GA_ROOT), nullptr, instance, &picker);
      if (popup) modal(popup, GetAncestor(window, GA_ROOT), 640, 328, false, control(extensionDrop));
      extensionLetter = picker.letter;
      if (!picker.selected.empty()) { set(extension, picker.selected.c_str()); reload(); }
      return;
    }
    if (id == IDCANCEL) { DestroyWindow(window); return; }
    if (id == originalLabel || id == commandLabel) {
      customView = id == commandLabel;
      updateView(); updateActions(); return;
    }
    if (id == refresh) { reload(before, row); return; }
    if (id == details && entry) {
      auto text = entry->label + L"\r\n" + entry->source;
      if (!entry->command.empty()) text += L"\r\n\r\n" + entry->command;
      ui::message(window, text.c_str(), lang::text(L"查看详情"), MB_OK | MB_ICONINFORMATION);
      return;
    }
    if (id == explanation && !statusLink()) return;
    if ((id == registryLocation || id == explanation) && entry) {
      const auto &source = entry->source;
      if (!source.starts_with(L"HKCU\\") && !source.starts_with(L"HKLM\\")) return;
      auto path = std::wstring(source.starts_with(L"HKCU") ? L"HKEY_CURRENT_USER" : L"HKEY_LOCAL_MACHINE") + source.substr(4);
      if (openingRegistry.valid()) return;
      std::packaged_task<DWORD()> launch([path = std::move(path)] {
        wchar_t windows[MAX_PATH]{};
        const auto length = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
        if (!length || length >= ARRAYSIZE(windows)) return DWORD(ERROR_PATH_NOT_FOUND);
        const auto regedit = std::filesystem::path(windows) / L"regedit.exe";
        // LastKey is the localized tree path, including the root above the hives.
        constexpr UINT computerCaption = 19;
        wchar_t root[256]{};
        if (auto resources = LoadLibraryExW(regedit.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
          LoadStringW(resources, computerCaption, root, ARRAYSIZE(root));
          FreeLibrary(resources);
        }
        if (!*root) return DWORD(ERROR_RESOURCE_NAME_NOT_FOUND);
        const auto lastKey = std::wstring(root) + L"\\" + path;
        constexpr auto key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Regedit";
        const auto result = RegSetKeyValueW(HKEY_CURRENT_USER, key, L"LastKey", REG_SZ, root,
            static_cast<DWORD>((wcslen(root) + 1) * sizeof(wchar_t)));
        if (result != ERROR_SUCCESS) return static_cast<DWORD>(result);
        SHELLEXECUTEINFOW run{sizeof(run)};
        run.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
        run.lpFile = regedit.c_str();
        run.lpParameters = L"-m";
        run.nShow = SW_SHOWNORMAL;
        const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const DWORD error = ShellExecuteExW(&run) ? ERROR_SUCCESS : GetLastError();
        if (SUCCEEDED(apartment)) CoUninitialize();
        if (error != ERROR_SUCCESS) return error;
        if (!run.hProcess) return DWORD(ERROR_NOT_READY);
        struct Process { HANDLE handle; ~Process() { CloseHandle(handle); } } process{run.hProcess};
        if (WaitForInputIdle(process.handle, 5000) != 0) return DWORD(ERROR_TIMEOUT);
        struct Address { DWORD process; HWND field; } address{GetProcessId(process.handle), nullptr};
        EnumWindows([](HWND candidate, LPARAM data) -> BOOL {
          auto &address = *reinterpret_cast<Address *>(data);
          DWORD pid = 0;
          GetWindowThreadProcessId(candidate, &pid);
          if (pid == address.process && IsWindowVisible(candidate))
            address.field = FindWindowExW(candidate, nullptr, L"Edit", nullptr);
          return !address.field;
        }, reinterpret_cast<LPARAM>(&address));
        if (!address.field) return DWORD(ERROR_NOT_READY);
        DWORD_PTR accepted = 0;
        if (!SendMessageTimeoutW(address.field, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(lastKey.c_str()),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 1500, &accepted) ||
            !accepted || !PostMessageW(address.field, WM_KEYDOWN, VK_RETURN, 1))
          return DWORD(ERROR_ACCESS_DENIED);
        return DWORD(ERROR_SUCCESS);
      });
      if (!SetTimer(window, registryTimer, 100, nullptr)) fn::fail(L"无法打开注册表位置。");
      try {
        auto result = launch.get_future();
        std::thread(std::move(launch)).detach();
        openingRegistry = std::move(result);
      } catch (...) {
        KillTimer(window, registryTimer);
        throw;
      }
      return;
    }
    if (id == enable) {
      const bool enabled = SendDlgItemMessageW(window, enable, BM_GETCHECK, 0, 0) == BST_CHECKED;
      persist(enabled, custom);
      active = enabled;
      if (!enabled) restoreMany();
    } else if (id == restoreAll) {
      if (ui::message(window, lang::text(L"还原 ShibaKit 对已有菜单所做的修改？"), lang::text(title), MB_YESNO | MB_ICONQUESTION) == IDYES)
        restoreMany();
    } else if (id == visibility && entry && entry->canHide) {
      const bool grouped = entry->shellBlock && !entry->handlerRegistration;
      if (!entry->hidden) {
        const auto prompt = grouped ? L"隐藏此扩展提供的所有右键菜单项？" : active ? L"隐藏此菜单项会影响它适用的所有对象。继续？" :
            L"启用右键菜单管理并隐藏此菜单项？";
        if (!direct && ui::message(window, lang::text(prompt), lang::text(title), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        if (!active && entry->noteOperation < 0) { persist(true, custom); active = true; }
      }
      if (entry->noteOperation >= 0) {
        setNoteHidden(entry->noteOperation, !entry->hidden);
        auto &stored = inventoryItems[originalRows[before]];
        stored.hidden = !entry->hidden;
        filter(before, row);
        return;
      }
      const bool hide = !entry->hidden;
      context::setHidden(*entry, hide);
      auto setChildVisibility = [&](size_t parent, bool hidden) {
        if (const auto range = submenuEnds.find(parent); range != submenuEnds.end())
          for (size_t i = parent + 1; i < range->second; ++i) inventoryItems[i].hidden = hidden;
      };
      if (grouped) {
        for (size_t i = 0; i < inventoryItems.size(); ++i) {
          auto &stored = inventoryItems[i];
          if (stored.shellBlockId != entry->shellBlockId) continue;
          stored.hidden = hide;
          stored.managed = hide;
          setChildVisibility(i, hide);
        }
      } else {
        auto &stored = inventoryItems[originalRows[before]];
        stored.hidden = hide;
        stored.managed = hide || stored.managed;
        if (!hide) {
          const auto updated = context::inventory(scene, currentExtension);
          auto match = std::find_if(updated.begin(), updated.end(), [&](const auto &item) { return item.source == stored.source; });
          if (match != updated.end()) {
            stored.hidden = match->hidden;
            stored.managed = match->managed;
            stored.canHide = match->canHide;
          }
          if (const auto children = submenuEnds.find(originalRows[before]); children != submenuEnds.end()) {
            for (size_t i = children->first + 1; i < children->second; ++i) {
              auto &child = inventoryItems[i];
              if (match == updated.end()) { child.hidden = false; continue; }
              const auto current = std::find_if(match->children.begin(), match->children.end(), [&](const auto &item) {
                return !child.source.empty() && child.source == item.source;
              });
              child.hidden = current != match->children.end() && current->hidden;
            }
          }
        } else setChildVisibility(originalRows[before], true);
      }
      SendDlgItemMessageW(window, enable, BM_SETCHECK, active ? BST_CHECKED : BST_UNCHECKED, 0);
      filter(before, row);
      return;
    } else if (id == restore && entry && entry->managed) {
      context::restore(*entry);
    } else if (id == appearance && active && entry && entry->canEdit) {
      Command values;
      values.name = entry->label; values.program = entry->icon;
      if (editCommand(instance, window, values, true)) context::setAppearance(*entry, values.name, values.program);
    } else if ((id == add || (id == copyCommand && entry && !entry->program.empty())) && customScene()) {
      Command value;
      value.scene = scene;
      if (scene == Scene::Extension) value.extension = currentExtension;
      if (id == copyCommand) {
        value.name = entry->label; value.program = entry->program; value.arguments = entry->arguments; value.icon = entry->icon;
      }
      if (editCommand(instance, window, value)) {
        auto revised = custom; revised.push_back(std::move(value));
        persist(active, revised);
        row = static_cast<int>(displayed.size());
        customView = true;
      }
    } else if (customSelected) {
      auto revised = custom;
      auto position = displayed[row];
      if (id == edit || (id == commands && code == LBN_DBLCLK)) {
        if (!editCommand(instance, window, revised[position])) return;
      } else if (id == remove) {
        if (ui::message(window, lang::text(L"删除这条自定义命令？"), lang::text(title), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        revised.erase(revised.begin() + position);
      } else if (id == toggle) {
        if (!direct && revised[position].enabled &&
            ui::message(window, lang::text(L"隐藏此菜单项会影响它适用的所有对象。继续？"),
                        lang::text(title), MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        revised[position].enabled = !revised[position].enabled;
      }
      else if (searchText.empty() && ((id == up && row > 0) || (id == down && size_t(row + 1) < displayed.size()))) {
        row += id == up ? -1 : 1;
        std::swap(revised[position], revised[displayed[row]]);
      } else return;
      persist(active, revised);
    } else return;
    reload(before, row);
  }
};

LRESULT CALLBACK managerProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) {
    auto state = static_cast<std::unique_ptr<Manager> *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams)->release();
    state->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  auto *state = reinterpret_cast<Manager *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return DefWindowProcW(window, message, w, l);
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(window, &paint);
    RECT bounds{};
    GetWindowRect(state->control(explanation), &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
    bounds.bottom = bounds.top - state->px(3);
    bounds.top = bounds.bottom - 1;
    FillRect(dc, &paint.rcPaint, state->theme.background);
    const auto &colors = state->theme.colors;
    SetDCBrushColor(dc, colors.border);
    FillRect(dc, &bounds, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (state->extensionScene()) {
      GetWindowRect(state->control(extension), &bounds);
      MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
      InflateRect(&bounds, state->px(6), state->px(5));
      ui::rounded(dc, bounds, colors.surface, colors.border, state->px(6));
    }
    GetWindowRect(state->control(menuSearch), &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&bounds), 2);
    bounds.left -= state->px(27);
    bounds.right += state->px(6);
    InflateRect(&bounds, 0, state->px(4));
    ui::rounded(dc, bounds, colors.field, colors.border, state->px(6));
    bounds.right = bounds.left + state->px(26);
    auto old = SelectObject(dc, state->symbols);
    ui::text(dc, L"\xE721", bounds, colors.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    EndPaint(window, &paint);
    return 0;
  }
  if (message == WM_NCDESTROY) {
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    state->destroyTips();
    delete state;
    return DefWindowProcW(window, message, w, l);
  }
  if (message == WM_APP + 31) {
    state->theme.reset(static_cast<ui::ThemeMode>(w)); state->theme.apply(window);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    return 0;
  }
  if (message == WM_CTLCOLORLISTBOX) {
    const auto field = reinterpret_cast<HWND>(l);
    if (field == state->control(category) || field == state->control(originals) ||
        field == state->control(commands)) {
      auto dc = reinterpret_cast<HDC>(w);
      SetBkColor(dc, state->theme.colors.background);
      SetTextColor(dc, state->theme.colors.text);
      return reinterpret_cast<LRESULT>(state->theme.background);
    }
  }
  if (message == WM_DRAWITEM && (w == category || w == originals || w == commands)) {
    state->drawMenuItem(*reinterpret_cast<const DRAWITEMSTRUCT *>(l)); return TRUE;
  }
  if (message == WM_MEASUREITEM && !w) {
    auto &item = *reinterpret_cast<MEASUREITEMSTRUCT *>(l);
    if (item.CtlType == ODT_MENU && item.itemData) { state->measurePopupRow(item); return TRUE; }
  }
  if (message == WM_DRAWITEM && !w) {
    const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(l);
    if (item.CtlType == ODT_MENU && item.itemData) { state->drawPopupRow(item); return TRUE; }
  }
  if (message == WM_NOTIFY && reinterpret_cast<NMHDR *>(l)->code == TTN_GETDISPINFOW) {
    auto *info = reinterpret_cast<NMTTDISPINFOW *>(l);
    if (info->hdr.hwndFrom == state->tips) {
      auto child = reinterpret_cast<HWND>(info->hdr.idFrom);
      state->tipText = child == state->control(explanation) ? state->statusText : windowText(child);
      info->lpszText = state->tipText.data();
      return 0;
    }
  }
  if (message == WM_PAINT) { state->paintDividers(); return 0; }
  if (message == WM_SETCURSOR && LOWORD(l) == HTCLIENT && reinterpret_cast<HWND>(w) == window) {
    POINT point{};
    GetCursorPos(&point); ScreenToClient(window, &point);
    if (Manager::dividerCursor(state->dragging != Manager::Divider::None ? state->dragging : state->dividerAt(point)))
      return TRUE;
  }
  if (message == WM_LBUTTONDOWN) {
    const auto divider = state->dividerAt({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
    if (divider != Manager::Divider::None) {
      state->dragging = divider;
      SetCapture(window);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
  }
  if (message == WM_MOUSEMOVE) {
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    if (state->dragging != Manager::Divider::None) state->resizeDivider(point);
    else {
      auto hover = state->dividerAt(point);
      if (state->hoveredDivider != hover) {
        state->hoveredDivider = hover;
        InvalidateRect(window, nullptr, FALSE);
      }
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
      TrackMouseEvent(&track);
    }
    return 0;
  }
  if (message == WM_LBUTTONUP && GetCapture() == window) { ReleaseCapture(); return 0; }
  if (message == WM_CAPTURECHANGED || message == WM_MOUSELEAVE) {
    if (message == WM_CAPTURECHANGED) state->dragging = Manager::Divider::None;
    state->hoveredDivider = Manager::Divider::None;
    InvalidateRect(window, nullptr, FALSE);
    return 0;
  }
  LRESULT themed = 0;
  if (state->theme.handle(window, message, w, l, themed)) return themed;
  try {
    if (message == WM_CREATE) {
      state->rescale(GetDpiForWindow(window)); state->createControls(); state->updateTheme();
      state->prepareActions(); state->layout(); return 0;
    }
    if (message == ui::themeChangedMessage() || message == WM_THEMECHANGED) { state->updateTheme(); return 0; }
    if (message == WM_GETMINMAXINFO) {
      auto *limits = reinterpret_cast<MINMAXINFO *>(l);
      limits->ptMinTrackSize = {state->px(820), state->px(540)};
      return 0;
    }
    if (message == WM_SIZE) { state->layout(); return 0; }
    if (message == WM_TIMER && w == Manager::loadTimer) { state->finishLoad(); return 0; }
    if (message == WM_TIMER && w == Manager::registryTimer) {
      auto &result = state->openingRegistry;
      if (result.valid() && result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        KillTimer(window, Manager::registryTimer);
        const DWORD error = result.get();
        if (error != ERROR_SUCCESS && error != ERROR_CANCELLED) fn::fail(L"无法打开注册表位置。", error);
      }
      return 0;
    }
    if (message == WM_SHOWWINDOW && w) { state->reload(); return 0; }
    if (message == WM_DPICHANGED_AFTERPARENT) { state->rescale(GetDpiForWindow(window)); state->layout(); return 0; }
    if (message == WM_DPICHANGED) {
      state->rescale(HIWORD(w));
      const auto &rect = *reinterpret_cast<RECT *>(l);
      SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
      state->layout();
      return 0;
    }
    if (message == WM_COMMAND) { state->handle(LOWORD(w), HIWORD(w), l == directEyeClick); return 0; }
  } catch (const fn::Error &error) {
    if (message == WM_CREATE) return -1;
    ui::message(window, error.message.c_str(), lang::text(title), MB_OK | MB_ICONERROR);
    SendDlgItemMessageW(window, enable, BM_SETCHECK, state->active ? BST_CHECKED : BST_UNCHECKED, 0);
  } catch (...) {
    if (message == WM_CREATE) return -1;
    ui::message(window, lang::text(L"右键菜单操作失败。"), lang::text(title), MB_OK | MB_ICONERROR);
    SendDlgItemMessageW(window, enable, BM_SETCHECK, state->active ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  return DefWindowProcW(window, message, w, l);
}
}

HWND createPage(HINSTANCE instance, HWND owner, void (*setNoteHidden)(int, bool)) {
  WNDCLASSW cls{};
  cls.hInstance = instance; cls.lpszClassName = managerClass; cls.lpfnWndProc = managerProc;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&cls);
  auto state = std::make_unique<Manager>(instance);
  state->setNoteHidden = setNoteHidden;
  return CreateWindowExW(WS_EX_CONTROLPARENT, managerClass, lang::text(title),
      WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, owner, nullptr, instance, &state);
}
void setTheme(HWND page, unsigned mode) { SendMessageW(page, WM_APP + 31, mode, 0); }
}
