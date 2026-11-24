#include "navigation.h"
#include "core.h"
#include "window_position.h"
#include "resource.h"
#include "quick_folders.h"
#include "quick_find_external.h"
#include "theme_scroll.h"
#include "app_version.h"
#include "update.h"
#include "context_manager.h"
#include "desktop_groups.h"
#include "desktop_groups_menu.h"
#include "desktop_groups_render.h"
#include "explorer_entries_page.h"
#include "language.h"
#include "navigation_registration.h"
#include <commdlg.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>
#include <memory>
#include <optional>
#include <future>
#include <thread>
#include <windowsx.h>
#define MessageBoxW ui::message
namespace navigation {
static constexpr wchar_t configKey[] = L"Software\\FolderNote";
static constexpr wchar_t listenerClass[] = L"FolderNote.Navigation.Listener.v1";
struct Notification { DWORD process; UINT message; };
static BOOL CALLBACK notifyTab(HWND window, LPARAM message) {
  const auto &notification = *reinterpret_cast<const Notification *>(message);
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process == notification.process && GetPropW(window, L"FolderNote.Navigation.Controller.v3"))
    PostMessageW(window, notification.message, 0, 0);
  return TRUE;
}

// Migration only: a 1.2.x listener may still be running during an upgrade.
// This version never creates a mouse hook or a background listener process.
void stopLegacy() {
  HWND h = FindWindowExW(HWND_MESSAGE, nullptr, listenerClass, nullptr);
  if (!h)
    return;
  DWORD pid = 0;
  GetWindowThreadProcessId(h, &pid);
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  PostMessageW(h, WM_CLOSE, 0, 0);
  if (process) {
    WaitForSingleObject(process, 3000);
    CloseHandle(process);
  }
}
static BOOL CALLBACK notifyWindow(HWND window, LPARAM message) {
  wchar_t cls[128]{};
  GetClassNameW(window, cls, 128);
  if (wcscmp(cls, L"CabinetWClass") && wcscmp(cls, L"ExploreWClass"))
    return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(window, &pid);
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return TRUE;
  wchar_t image[32768]{}, windows[MAX_PATH]{};
  DWORD length = ARRAYSIZE(image);
  BOOL ok = QueryFullProcessImageNameW(process, 0, image, &length);
  CloseHandle(process);
  if (ok && GetWindowsDirectoryW(windows, ARRAYSIZE(windows)) &&
      _wcsicmp(image, (std::wstring(windows) + L"\\explorer.exe").c_str()) == 0) {
    PostMessageW(window, static_cast<UINT>(message), 0, 0);
    const Notification notification{pid, static_cast<UINT>(message)};
    EnumChildWindows(window, notifyTab, reinterpret_cast<LPARAM>(&notification));
  }
  return TRUE;
}
static void notifyExplorer(const wchar_t *name) {
  UINT message = RegisterWindowMessageW(name);
  if (message)
    EnumWindows(notifyWindow, static_cast<LPARAM>(message));
}
void stop() {
  stopLegacy();
  notifyExplorer(L"FolderNote.Navigation.Shutdown.v2");
}
void reload() {
  stopLegacy();
  notifyExplorer(L"FolderNote.Navigation.SettingsChanged.v2");
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
namespace {
struct SettingValue {
  HKEY key = nullptr;
  const wchar_t *name = nullptr;
  DWORD type = 0;
  std::vector<BYTE> bytes;
  bool existed = false;
  bool changed = false;

  void read(HKEY source, const wchar_t *valueName) {
    key = source;
    name = valueName;
    DWORD count = 0;
    auto error = RegQueryValueExW(key, name, nullptr, &type, nullptr, &count);
    if (error == ERROR_FILE_NOT_FOUND)
      return;
    if (error != ERROR_SUCCESS)
      fn::fail(L"无法读取现有设置", error);
    existed = true;
    bytes.resize(count);
    error = RegQueryValueExW(key, name, nullptr, &type, bytes.data(), &count);
    if (error != ERROR_SUCCESS)
      fn::fail(L"无法读取现有设置", error);
    bytes.resize(count);
  }
  LSTATUS restore() const {
    if (!changed)
      return ERROR_SUCCESS;
    const auto error = existed ? RegSetValueExW(key, name, 0, type, bytes.data(),
                                               static_cast<DWORD>(bytes.size()))
                               : RegDeleteValueW(key, name);
    return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error;
  }
};
struct SettingKeys {
  std::array<HKEY, 4> values{};
  ~SettingKeys() {
    for (auto key : values)
      if (key)
        RegCloseKey(key);
  }
};
} // namespace

struct DesktopGroupsSettings {
  bool enabled, glass;
  DWORD radius = 10, opacity = 55;
  bool shadow = false;
  DWORD shadowOpacity = 36, shadowBlur = 12, shadowOffset = 3;
};
static bool configuredDesktopGroupsGlass() {
  return desktop_groups::configuredAppearance().glass;
}
static void applySettings(bool notes, Action action, Action preferred, bool quick, bool dialogs, ui::ThemeMode theme, bool tabs,
                          bool preferNewTab, bool saveConfiguration, std::optional<DesktopGroupsSettings> desktopGroups = std::nullopt) {
  if (action != Action::Off && action != Action::Parent && action != Action::Back)
    throw fn::Error{lang::text(L"请选择有效的双击操作。")};
  if (preferred != Action::Parent && preferred != Action::Back)
    throw fn::Error{lang::text(L"请选择有效的双击操作。")};
  if (DWORD(theme) > 2) throw fn::Error{lang::text(L"请选择有效的主题。")};
  SettingKeys keys;
  LSTATUS error = ERROR_SUCCESS;
  constexpr size_t configurationCount = 16;
  std::array<SettingValue, configurationCount + 3> saved;
  if (saveConfiguration) {
    error = RegCreateKeyExW(HKEY_CURRENT_USER, configKey, 0, nullptr, 0,
                               KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &keys.values[0], nullptr);
    if (error)
      fn::fail(L"无法保存设置", error);
    saved[0].read(keys.values[0], L"NotesEnabled");
    saved[1].read(keys.values[0], L"BlankDoubleClick");
    saved[2].read(keys.values[0], L"NavigationPreference");
    saved[3].read(keys.values[0], L"QuickFoldersEnabled");
    saved[4].read(keys.values[0], L"QuickFoldersInDialogs");
    saved[5].read(keys.values[0], L"Theme");
    saved[6].read(keys.values[0], L"ExplorerTabsEnabled");
    saved[7].read(keys.values[0], L"ExplorerOpenInNewTab");
    if (desktopGroups) {
      saved[8].read(keys.values[0], L"DesktopGroupsEnabled");
      saved[9].read(keys.values[0], L"DesktopGroupsGlass");
      saved[10].read(keys.values[0], L"DesktopGroupsRadius");
      saved[11].read(keys.values[0], L"DesktopGroupsOpacity");
      saved[12].read(keys.values[0], L"DesktopGroupsShadow");
      saved[13].read(keys.values[0], L"DesktopGroupsShadowOpacity");
      saved[14].read(keys.values[0], L"DesktopGroupsShadowBlur");
      saved[15].read(keys.values[0], L"DesktopGroupsShadowOffset");
    }
  }
  // Only alter menus already installed by FolderNote. A modern-only or portable
  // installation must not gain incomplete legacy menu entries when saving.
  for (size_t index = 0; index < 3; ++index) {
    const auto path = std::wstring(L"Software\\Classes\\Directory\\shell\\") + fn::verbs[index];
    error = RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                          &keys.values[index + 1]);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      continue;
    if (error)
      fn::fail(L"无法更新备注菜单开关", error);
    saved[index + configurationCount].read(keys.values[index + 1], L"LegacyDisable");
  }
  const std::array<DWORD, configurationCount> values{notes ? 1UL : 0UL, DWORD(action), DWORD(preferred), quick ? 1UL : 0UL, dialogs ? 1UL : 0UL,
      DWORD(theme), tabs ? 1UL : 0UL, preferNewTab ? 1UL : 0UL,
      desktopGroups && desktopGroups->enabled ? 1UL : 0UL, desktopGroups && desktopGroups->glass ? 1UL : 0UL,
      desktopGroups ? desktopGroups->radius : 0UL, desktopGroups ? desktopGroups->opacity : 0UL,
      desktopGroups && desktopGroups->shadow ? 1UL : 0UL,
      desktopGroups ? desktopGroups->shadowOpacity : 0UL, desktopGroups ? desktopGroups->shadowBlur : 0UL,
      desktopGroups ? desktopGroups->shadowOffset : 0UL};
  for (size_t index = 0; index < saved.size(); ++index) {
    auto &value = saved[index];
    if (!value.key)
      continue;
    if (index < values.size())
      error = RegSetValueExW(value.key, value.name, 0, REG_DWORD,
                             reinterpret_cast<const BYTE *>(&values[index]), sizeof(DWORD));
    else if (notes && !noteCommandHidden(static_cast<int>(index - values.size()))) {
      error = RegDeleteValueW(value.key, value.name);
      if (error == ERROR_FILE_NOT_FOUND)
        error = ERROR_SUCCESS;
    } else {
      const wchar_t empty[] = L"";
      error = RegSetValueExW(value.key, value.name, 0, REG_SZ,
                             reinterpret_cast<const BYTE *>(empty), sizeof(empty));
    }
    if (error != ERROR_SUCCESS) {
      bool restored = true;
      for (const auto &previous : saved)
        restored = previous.restore() == ERROR_SUCCESS && restored;
      if (!restored)
        throw fn::Error{lang::text(L"设置未能完整保存或恢复，请重新打开设置检查功能开关。")};
      fn::fail(L"设置未保存，已恢复原有开关", error);
    }
    value.changed = true;
  }
}
void saveSettings(bool notes, Action action, Action preferred) {
  saveSettings(notes, action, preferred, quickEnabled(), dialogQuickEnabled());
}
void setNoteCommandHidden(int operation, bool hidden) {
  if (operation < 0 || operation >= 3) return;
  DWORD previous = 0, bytes = sizeof(previous);
  RegGetValueW(HKEY_CURRENT_USER, configKey, L"HiddenNoteCommands", RRF_RT_REG_DWORD, nullptr, &previous, &bytes);
  const DWORD bit = 1u << operation;
  const DWORD value = hidden ? previous | bit : previous & ~bit;
  const auto error = RegSetKeyValueW(HKEY_CURRENT_USER, configKey, L"HiddenNoteCommands", REG_DWORD, &value, sizeof(value));
  if (error) fn::fail(L"无法保存设置", error);
  try { reload(); } catch (...) {
    RegSetKeyValueW(HKEY_CURRENT_USER, configKey, L"HiddenNoteCommands", REG_DWORD, &previous, sizeof(previous));
    throw;
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs) {
  saveSettings(notes, action, preferred, quick, dialogs, ui::configuredTheme());
}
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs, ui::ThemeMode theme) {
  saveSettings(notes, action, preferred, quick, dialogs, theme, tabsEnabled());
}
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs, ui::ThemeMode theme, bool tabs) {
  saveSettings(notes, action, preferred, quick, dialogs, theme, tabs, preferNewTabEnabled());
}
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs, ui::ThemeMode theme, bool tabs, bool preferNewTab) {
  applySettings(notes, action, preferred, quick, dialogs, theme, tabs, preferNewTab, true);
}
void syncNoteMenus() {
  applySettings(notesEnabled(), configuredAction(), preferredAction(), quickEnabled(), dialogQuickEnabled(), ui::configuredTheme(), tabsEnabled(), preferNewTabEnabled(), false);
}
static HWND choice;
static HWND notesCheck;
static HWND navigationCheck;
static HWND quickCheck;
static HWND dialogCheck;
static HWND tabsCheck;
static HWND preferNewTabCheck;
static HWND desktopGroupsCheck, desktopGroupsManager, desktopGroupsGlassCheck, desktopGroupsPreview;
static HFONT font, headingFont;
static HWND body, themeChoice, settingsWindow, managerPage, contextPage, entriesPage;
static SettingsPage activePage = SettingsPage::Features;
static int settingsSidebarWidth = 208;
static bool draggingSettingsSidebar = false;
static HWND updateTip;
static constexpr std::array pageButtons{40, 41, 47, 49, 48, 50, 75};
static const wchar_t *pageTitle(size_t page) {
  constexpr std::array labels{L"功能设置", L"置顶与历史", L"右键菜单管理", L"文件夹入口管理", L"设置", L"盒子样式", L"快捷搜索"};
  return lang::text(labels[page]);
}
static UINT selectPageMessage() { return RegisterWindowMessageW(L"FolderNote.Settings.SelectPage.v1"); }
static std::unique_ptr<ui::Theme> appearance;
static constexpr int languageChoice = 43, languageExport = 44, languageImport = 45;
static constexpr int desktopGroupsToggle = 22, desktopGroupsManage = 23, desktopGroupsGlassToggle = 24, desktopGroupsPreviewId = 25;
static constexpr int boxGlass = 51, boxShadow = 52, boxRadius = 53, boxRadiusLabel = 55,
                     boxPreview = 57, boxPreviewLabel = 58, boxStyleShortcut = 59;
static constexpr int boxShadowOpacity = 60, boxShadowBlur = 61, boxShadowOffset = 62;
static constexpr int boxShadowOpacityLabel = 63, boxShadowBlurLabel = 64, boxShadowOffsetLabel = 65;
struct BoxSlider {
  int control, label;
  const wchar_t *name, *valueLabel;
  DWORD desktop_groups::Appearance::*value;
  int min, max;
  bool desktop_groups::Appearance::*enabled;
};
static constexpr BoxSlider boxSliders[]{
    {boxRadius, boxRadiusLabel, L"圆角", L"圆角  {0} px", &desktop_groups::Appearance::radius, 0, 20, nullptr},
    {boxShadowOpacity, boxShadowOpacityLabel, L"投影不透明度", L"投影不透明度  {0}%",
     &desktop_groups::Appearance::shadowOpacity, 0, 100, &desktop_groups::Appearance::shadow},
    {boxShadowBlur, boxShadowBlurLabel, L"投影模糊", L"投影模糊  {0} px",
     &desktop_groups::Appearance::shadowBlur, 1, 32, &desktop_groups::Appearance::shadow},
    {boxShadowOffset, boxShadowOffsetLabel, L"垂直偏移", L"垂直偏移  {0} px",
     &desktop_groups::Appearance::shadowOffset, 0, 20, &desktop_groups::Appearance::shadow},
};
static desktop_groups::Appearance boxStyle;
static bool boxAppearancePreviewing = false;
static bool reopenForLanguage = false;
struct SettingsDraft {
  std::array<LRESULT, 8> toggles;
  LRESULT action;
  ui::ThemeMode theme;
  desktop_groups::Appearance boxStyle;
};
static std::optional<SettingsDraft> languageDraft;

static void updateDesktopGroupsManager() {
  const bool boxesChecked = SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
  EnableWindow(desktopGroupsManager, desktop_groups::enabled() && boxesChecked);
}

static void refreshLanguageMenus() {
  for (size_t i = 0; i < std::size(fn::verbs); ++i) {
    auto path = std::wstring(L"Software\\Classes\\Directory\\shell\\") + fn::verbs[i];
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) continue;
    auto title = lang::text(fn::titles[i]);
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(title),
                   static_cast<DWORD>((wcslen(title) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
  }
  if (desktop_groups::enabled()) {
    try { desktop_groups::configureDesktopMenu(quick::executable()); } catch (const fn::Error &) {}
  }
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

static std::wstring languagePath(HWND owner, bool importing) {
  std::wstring path(32768, L'\0');
  if (!importing) wcscpy_s(path.data(), path.size(), L"FolderNote-language.xml");
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner = owner;
  dialog.lpstrFilter = L"XML (*.xml)\0*.xml\0\0";
  dialog.lpstrFile = path.data();
  dialog.nMaxFile = static_cast<DWORD>(path.size());
  dialog.lpstrDefExt = L"xml";
  dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (importing ? OFN_FILEMUSTEXIST : OFN_OVERWRITEPROMPT);
  if (!(importing ? GetOpenFileNameW(&dialog) : GetSaveFileNameW(&dialog))) {
    if (CommDlgExtendedError()) throw std::runtime_error("File picker failed");
    return {};
  }
  path.resize(wcslen(path.c_str()));
  return path;
}
static constexpr int updateButton = 42, automaticUpdateCheck = 70, automaticUpdateDescription = 71, updateStatusLabel = 72;
static constexpr int findDescription = 80, findScope = 81, findBoxes = 82, findLists = 83,
    findExplorer = 84, findDialogs = 85, findFolderLabel = 86, findFolderAction = 87,
    findFileLabel = 88, findFileAction = 89, findPositionLabel = 90, findPosition = 91,
    findRowsLabel = 92, findRows = 93, findIcons = 94, findHint = 95;
struct FindToggle { int id; const wchar_t *label; bool quick_find::Settings::*field; };
static constexpr FindToggle findToggles[]{
    {findBoxes, L"桌面盒子", &quick_find::Settings::boxes},
    {findLists, L"软件内列表与中键菜单", &quick_find::Settings::lists},
    {findExplorer, L"桌面与资源管理器", &quick_find::Settings::explorer},
    {findDialogs, L"系统打开、保存和文件夹窗口", &quick_find::Settings::dialogs},
    {findIcons, L"显示原生图标", &quick_find::Settings::showIcons}};
static void refreshFindSettings(HWND window) {
  const auto options = quick_find::settings();
  for (const auto &toggle : findToggles)
    SendDlgItemMessageW(window, toggle.id, BM_SETCHECK, options.*toggle.field ? BST_CHECKED : BST_UNCHECKED, 0);
  SendDlgItemMessageW(window, findFolderAction, CB_SETCURSEL, options.openFolders ? 1 : 0, 0);
  SendDlgItemMessageW(window, findFileAction, CB_SETCURSEL, options.openFiles ? 1 : 0, 0);
  SendDlgItemMessageW(window, findPosition, CB_SETCURSEL, options.atPointer ? 0 : 1, 0);
  SendDlgItemMessageW(window, findRows, CB_SETCURSEL, options.visibleRows - 5, 0);
}
static void saveFindSettings(HWND window) {
  const auto previous = quick_find::settings();
  auto options = previous;
  for (const auto &toggle : findToggles)
    options.*toggle.field = SendDlgItemMessageW(window, toggle.id, BM_GETCHECK, 0, 0) == BST_CHECKED;
  options.openFolders = SendDlgItemMessageW(window, findFolderAction, CB_GETCURSEL, 0, 0) == 1;
  options.openFiles = SendDlgItemMessageW(window, findFileAction, CB_GETCURSEL, 0, 0) == 1;
  options.atPointer = SendDlgItemMessageW(window, findPosition, CB_GETCURSEL, 0, 0) == 0;
  options.visibleRows = 5 + static_cast<unsigned>(SendDlgItemMessageW(window, findRows, CB_GETCURSEL, 0, 0));
  const auto error = quick_find::saveSettings(options);
  if (error != ERROR_SUCCESS) fn::fail(L"无法保存快捷搜索设置。", error);
  if (options.explorer != previous.explorer || options.dialogs != previous.dialogs)
    quick::syncHelper(quick::executable());
}
static constexpr UINT_PTR updateTimer = 42;
static constexpr auto automaticUpdateValue = L"AutomaticUpdate";
static constexpr auto lastAutomaticCheckValue = L"LastAutomaticUpdateCheck";
static constexpr auto uninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FolderNote";

enum class UpdateOperation { Check, Install, Automatic };
struct UpdateResult {
  std::optional<updater::Release> release;
  std::wstring error;
  bool installed = false;
};
static std::future<UpdateResult> updateWork;
static UpdateOperation updateOperation = UpdateOperation::Check;
static std::optional<updater::Release> availableUpdate;
static std::wstring updateStatus, updatedVersion;

static bool automaticUpdatesEnabled() {
  DWORD enabled = 0, bytes = sizeof(enabled);
  return RegGetValueW(HKEY_CURRENT_USER, configKey, automaticUpdateValue, RRF_RT_REG_DWORD,
                     nullptr, &enabled, &bytes) == ERROR_SUCCESS && enabled == 1;
}

static ULONGLONG updateClock() {
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  return (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

static bool automaticCheckDue(ULONGLONG now) {
  constexpr ULONGLONG day = 24ULL * 60 * 60 * 10000000;
  ULONGLONG previous = 0;
  DWORD bytes = sizeof(previous);
  return RegGetValueW(HKEY_CURRENT_USER, configKey, lastAutomaticCheckValue, RRF_RT_REG_QWORD,
                      nullptr, &previous, &bytes) != ERROR_SUCCESS ||
      previous > now || now - previous >= day;
}

static bool modernInstallation() {
  UINT32 packageLength = 0;
  if (GetCurrentPackageFullName(&packageLength, nullptr) == ERROR_INSUFFICIENT_BUFFER) return true;
  if (RegGetValueW(HKEY_CURRENT_USER, uninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
    return false;
  // The Win11 installer removes the classic verbs because its MSIX owns them.
  HKEY key = nullptr;
  auto result = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\FolderNote.Edit", 0, KEY_QUERY_VALUE, &key);
  if (key) RegCloseKey(key);
  return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
}

static bool canInstallAutomatically() {
  if (!automaticUpdatesEnabled() || modernInstallation()) return false;
  wchar_t version[32]{}, command[32768]{};
  DWORD versionBytes = sizeof(version), commandBytes = sizeof(command);
  if (RegGetValueW(HKEY_CURRENT_USER, uninstallKey, L"DisplayVersion", RRF_RT_REG_SZ,
                   nullptr, version, &versionBytes) != ERROR_SUCCESS ||
      std::wstring_view(version) != app::version ||
      RegGetValueW(HKEY_CURRENT_USER, uninstallKey, L"UninstallString", RRF_RT_REG_SZ,
                   nullptr, command, &commandBytes) != ERROR_SUCCESS) return false;
  const auto expected = L"\"" + (fn::modulePath().parent_path() / L"Uninstall.exe").native() + L"\"";
  return _wcsicmp(command, expected.c_str()) == 0 && !navreg::requiresMachineRegistration();
}

static bool installingManually() { return updateWork.valid() && updateOperation == UpdateOperation::Install; }
static bool workingAutomatically() { return updateWork.valid() && updateOperation == UpdateOperation::Automatic; }

static void updateControls(HWND owner) {
  const HWND button = GetDlgItem(owner, updateButton);
  const auto label = updateWork.valid()
      ? lang::text(installingManually() ? L"正在下载并安装…" : L"正在检查更新…")
      : updateStatus.empty() ? lang::text(L"检查更新") : updateStatus.c_str();
  SetWindowTextW(button, label);
  SetDlgItemTextW(owner, updateStatusLabel, updateStatus.c_str());
  if (updateTip) {
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = owner;
    tool.uId = reinterpret_cast<UINT_PTR>(button);
    tool.lpszText = const_cast<wchar_t *>(label);
    SendMessageW(updateTip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
  }
  const bool manualInstall = installingManually();
  EnableWindow(button, !updateWork.valid());
  EnableWindow(GetDlgItem(owner, automaticUpdateCheck), *app::updateRepository &&
               (!updateWork.valid() || workingAutomatically()));
  for (const int id : {IDOK, IDCANCEL, languageChoice, languageImport})
    EnableWindow(GetDlgItem(owner, id), !manualInstall);
  InvalidateRect(button, nullptr, TRUE);
}

static void startUpdateWork(HWND owner, UpdateOperation operation, std::optional<updater::Release> release = {}) {
  if (updateWork.valid()) return;
  try {
    const bool modern = operation != UpdateOperation::Install && modernInstallation();
    const auto version = updatedVersion.empty() ? std::wstring(app::version) : updatedVersion;
    std::packaged_task<UpdateResult()> task([operation, release = std::move(release), modern, version]() mutable {
      UpdateResult result;
      try {
        if (operation == UpdateOperation::Install) {
          updater::install(*release);
        } else {
          result.release = updater::check(app::updateRepository, version, modern);
          if (operation == UpdateOperation::Automatic && result.release && !modern && canInstallAutomatically())
            result.installed = updater::install(*result.release, canInstallAutomatically);
        }
      } catch (const fn::Error &error) { result.error = error.message; }
      catch (...) { result.error = lang::text(L"更新失败，请稍后重试。"); }
      return result;
    });
    auto future = task.get_future();
    if (!SetTimer(owner, updateTimer, 100, nullptr)) fn::fail(L"无法启动更新检查");
    std::thread(std::move(task)).detach();
    updateWork = std::move(future);
    updateOperation = operation;
    updateControls(owner);
  } catch (...) {
    KillTimer(owner, updateTimer);
    if (operation != UpdateOperation::Automatic)
      MessageBoxW(owner, lang::text(L"无法启动更新，请重试。"), lang::text(L"FolderNote 更新"), MB_OK | MB_ICONERROR);
  }
}

static void checkAutomatically(HWND owner) {
  if (!*app::updateRepository || updateWork.valid() || !automaticUpdatesEnabled()) return;
  const auto now = updateClock();
  if (!automaticCheckDue(now)) return;
  // Claim the check before starting network activity so a subsequent Settings launch
  // does not start again while this one is still checking or installing.
  if (RegSetKeyValueW(HKEY_CURRENT_USER, configKey, lastAutomaticCheckValue, REG_QWORD, &now, sizeof(now)) == ERROR_SUCCESS)
    startUpdateWork(owner, UpdateOperation::Automatic);
}

static std::wstring formatUpdateNotesText(const updater::Release &release) {
  if (!release.notes.empty()) return release.notes;
  return lang::text(L"暂无详细更新说明。");
}

static std::wstring formatUpdatePrompt(const updater::Release &release) {
  auto header = lang::format(L"发现新版本 FolderNote {0}（当前版本 {1}）。", {release.version, app::version});
  auto notesTitle = std::wstring(lang::text(L"更新内容："));
  auto notes = formatUpdateNotesText(release);
  auto question = std::wstring(lang::text(L"现在从 GitHub 下载并启动安装吗？"));
  return header + L"\r\n\r\n" + notesTitle + L"\r\n" + notes + L"\r\n\r\n" + question;
}

static constexpr int updateDialogHeading = 201, updateDialogNotesLabel = 202,
                     updateDialogNotesEdit = 203, updateDialogQuestion = 204;

struct UpdateConfirmDialogState {
  ui::Theme *theme = nullptr;
  HFONT textFont = nullptr;
  HFONT titleFont = nullptr;
  bool ownedTextFont = false;
  RECT notesFrame{};
  bool confirmed = false;
  bool closed = false;
  ~UpdateConfirmDialogState() {
    if (ownedTextFont && textFont) DeleteObject(textFont);
    if (titleFont) DeleteObject(titleFont);
  }
};

static LRESULT CALLBACK updateConfirmEditProc(HWND edit, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(edit, updateConfirmEditProc, id);
    return DefSubclassProc(edit, message, w, l);
  }
  if (message == WM_KEYDOWN) {
    if (w == VK_ESCAPE) {
      PostMessageW(GetParent(edit), WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
      return 0;
    }
    if (w == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
      PostMessageW(GetParent(edit), WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), 0);
      return 0;
    }
  }
  return DefSubclassProc(edit, message, w, l);
}

static LRESULT CALLBACK updateConfirmDialogProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto *state = reinterpret_cast<UpdateConfirmDialogState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    state = static_cast<UpdateConfirmDialogState *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (!state) return DefWindowProcW(window, message, w, l);
  if (message == WM_COMMAND) {
    const int id = LOWORD(w);
    if (id == IDYES || id == IDOK) {
      state->confirmed = true;
      state->closed = true;
      DestroyWindow(window);
      return 0;
    }
    if (id == IDNO || id == IDCANCEL) {
      state->confirmed = false;
      state->closed = true;
      DestroyWindow(window);
      return 0;
    }
  }
  if (message == WM_KEYDOWN) {
    if (w == VK_ESCAPE) {
      PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
      return 0;
    }
    if (w == VK_RETURN) {
      PostMessageW(window, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), 0);
      return 0;
    }
  }
  if (message == WM_CLOSE) {
    state->confirmed = false;
    state->closed = true;
    DestroyWindow(window);
    return 0;
  }
  if (message == WM_PAINT && state->theme) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, state->theme->background);
    if (!IsRectEmpty(&state->notesFrame)) {
      const int radius = MulDiv(6, GetDpiForWindow(window), 96);
      ui::rounded(dc, state->notesFrame, state->theme->colors.field, state->theme->colors.border, radius);
    }
    EndPaint(window, &paint);
    return 0;
  }
  LRESULT handled = 0;
  if (state->theme && state->theme->handle(window, message, w, l, handled)) return handled;
  return DefWindowProcW(window, message, w, l);
}

static HWND createUpdateConfirmDialog(HWND owner, const updater::Release &release, UpdateConfirmDialogState &state) {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  constexpr wchar_t dialogClass[] = L"FolderNote.UpdateConfirmDialog";
  WNDCLASSW cls{};
  if (!GetClassInfoW(instance, dialogClass, &cls)) {
    cls.lpfnWndProc = updateConfirmDialogProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_FOLDERNOTE));
    cls.lpszClassName = dialogClass;
    RegisterClassW(&cls);
  }

  const UINT windowDpi = owner && IsWindow(owner) ? GetDpiForWindow(owner) : GetDpiForSystem();
  const auto px = [windowDpi](int n) { return MulDiv(n, windowDpi, 96); };
  state.theme = appearance.get();
  state.titleFont = ui::makeFont(windowDpi, 13, FW_SEMIBOLD);
  state.textFont = ui::makeFont(windowDpi);
  state.ownedTextFont = true;

  const int dialogW = px(500);
  const int dialogH = px(380);
  RECT ownerRect{};
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  if (owner && IsWindow(owner) && GetWindowRect(owner, &ownerRect)) {
    x = ownerRect.left + ((ownerRect.right - ownerRect.left) - dialogW) / 2;
    y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - dialogH) / 2;
  }

  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, dialogClass,
                                lang::text(L"FolderNote 更新"),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                x, y, dialogW, dialogH, owner, nullptr, instance, &state);
  if (!dialog) return nullptr;

  RECT client{};
  GetClientRect(dialog, &client);
  const int margin = px(20);
  const int contentW = std::max(px(200), static_cast<int>(client.right) - margin * 2);

  const auto headingText = lang::format(L"发现新版本 FolderNote {0}（当前版本 {1}）", {release.version, app::version});
  HWND heading = CreateWindowExW(0, L"STATIC", headingText.c_str(),
                                 WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                 margin, px(18), contentW, px(24),
                                 dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(updateDialogHeading)), instance, nullptr);
  HWND notesLabel = CreateWindowExW(0, L"STATIC", lang::text(L"更新内容："),
                                    WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                    margin, px(48), contentW, px(20),
                                    dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(updateDialogNotesLabel)), instance, nullptr);
  ui::muted(notesLabel);

  const int buttonH = px(32);
  const int buttonW = px(112);
  const int bottomY = static_cast<int>(client.bottom) - margin - buttonH;
  const int questionY = bottomY - px(28);
  const int frameTop = px(72);
  const int frameBottom = std::max(frameTop + px(80), questionY - px(12));
  state.notesFrame = {margin, frameTop, margin + contentW, frameBottom};

  const int pad = px(8);
  const auto notesText = formatUpdateNotesText(release);
  HWND notesEdit = CreateWindowExW(0, L"EDIT", notesText.c_str(),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                   ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                                   state.notesFrame.left + pad, state.notesFrame.top + pad,
                                   std::max(px(60), contentW - pad * 2),
                                   std::max(px(40), (frameBottom - frameTop) - pad * 2),
                                   dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(updateDialogNotesEdit)), instance, nullptr);
  SetWindowSubclass(notesEdit, updateConfirmEditProc, 1, 0);

  HWND question = CreateWindowExW(0, L"STATIC", lang::text(L"现在从 GitHub 下载并启动安装吗？"),
                                  WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                                  margin, questionY, contentW, px(20),
                                  dialog, reinterpret_cast<HMENU>(static_cast<INT_PTR>(updateDialogQuestion)), instance, nullptr);

  const int gap = px(10);
  const int yesX = margin + contentW - buttonW * 2 - gap;
  const int noX = margin + contentW - buttonW;
  HWND yesBtn = CreateWindowExW(0, L"BUTTON", lang::text(L"立即更新"),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                yesX, bottomY, buttonW, buttonH,
                                dialog, reinterpret_cast<HMENU>(IDYES), instance, nullptr);
  HWND noBtn = CreateWindowExW(0, L"BUTTON", lang::text(L"暂不更新"),
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               noX, bottomY, buttonW, buttonH,
                               dialog, reinterpret_cast<HMENU>(IDNO), instance, nullptr);

  for (HWND ctrl : {notesLabel, notesEdit, question, yesBtn, noBtn}) {
    if (ctrl && state.textFont) SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(state.textFont), FALSE);
  }
  if (heading && (state.titleFont || state.textFont)) {
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(state.titleFont ? state.titleFont : state.textFont), FALSE);
  }
  SendMessageW(notesEdit, EM_SETSEL, static_cast<WPARAM>(-1), 0);
  if (state.theme) state.theme->apply(dialog);
  return dialog;
}

static bool confirmUpdateDialog(HWND owner, const updater::Release &release) {
  UpdateConfirmDialogState state;
  HWND dialog = createUpdateConfirmDialog(owner, release, state);
  if (!dialog) {
    const auto prompt = formatUpdatePrompt(release);
    return MessageBoxW(owner, prompt.c_str(), lang::text(L"FolderNote 更新"), MB_YESNO | MB_ICONQUESTION) == IDYES;
  }
  const bool hadOwner = owner && IsWindow(owner);
  const bool wasEnabled = hadOwner && IsWindowEnabled(owner) != FALSE;
  if (hadOwner && wasEnabled) EnableWindow(owner, FALSE);
  ShowWindow(dialog, SW_SHOWNORMAL);
  SetFocus(GetDlgItem(dialog, IDYES));
  MSG msg{};
  while (!state.closed && IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(dialog, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (hadOwner && IsWindow(owner)) {
    if (wasEnabled) EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  if (IsWindow(dialog)) DestroyWindow(dialog);
  return state.confirmed;
}

static void offerUpdate(HWND owner, const updater::Release &release) {
  if (confirmUpdateDialog(owner, release))
    startUpdateWork(owner, UpdateOperation::Install, release);
}

static void finishUpdateWork(HWND owner) {
  using namespace std::chrono_literals;
  if (!updateWork.valid() || updateWork.wait_for(0ms) != std::future_status::ready) return;
  KillTimer(owner, updateTimer);
  const auto operation = updateOperation;
  auto result = updateWork.get();
  if (operation == UpdateOperation::Automatic) {
    availableUpdate.reset();
    if (result.installed) {
      updatedVersion = result.release->version;
      updateStatus = lang::format(L"已安装 FolderNote {0}。重新打开设置即可使用新版。", {updatedVersion});
    } else if (result.release) {
      availableUpdate = std::move(result.release);
      updateStatus = lang::format(L"发现可用更新 {0}。", {availableUpdate->version});
      if (!result.error.empty()) updateStatus += L"\n" + result.error;
    } else {
      updateStatus = std::move(result.error);
    }
    updateControls(owner);
    return;
  }
  if (operation == UpdateOperation::Check && result.error.empty()) {
    availableUpdate = result.release;
    if (availableUpdate) updateStatus = lang::format(L"发现可用更新 {0}。", {availableUpdate->version});
    else if (updatedVersion.empty()) updateStatus.clear();
  }
  updateControls(owner);
  if (operation == UpdateOperation::Install && result.error.empty()) { DestroyWindow(owner); return; }
  if (!result.error.empty()) {
    MessageBoxW(owner, result.error.c_str(), lang::text(L"FolderNote 更新"), MB_OK | MB_ICONERROR);
  } else if (result.release) {
    offerUpdate(owner, *result.release);
  } else {
    MessageBoxW(owner, lang::text(L"当前已是最新版本。"), lang::text(L"FolderNote 更新"), MB_OK | MB_ICONINFORMATION);
  }
}
static int scrollPosition = 0;
static ui::ScrollBar settingsScroll;
struct Placement { HWND window; int x, y, width, height; };
static std::vector<Placement> placements;
static UINT dpi;
static int scale(int n) { return MulDiv(n, dpi, 96); }
static constexpr int desktopGroupsHeight = 230;
static constexpr int desktopGroupsPreviewTop = 537, desktopGroupsPreviewHeight = 120;
static int contentHeight() { return scale((tabsSupported() ? 634 : 440) + desktopGroupsHeight); }

static COLORREF blend(COLORREF a, COLORREF b, int weight) {
  const auto channel = [weight](int from, int to) { return (from * (100 - weight) + to * weight) / 100; };
  return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}

static void updateBoxStylePreview(HWND owner) {
  for (const auto &slider : boxSliders) {
    const auto label = lang::format(slider.valueLabel, {std::to_wstring(boxStyle.*slider.value)});
    SetDlgItemTextW(owner, slider.label, label.c_str());
    const bool enabled = !slider.enabled || boxStyle.*slider.enabled;
    EnableWindow(GetDlgItem(owner, slider.control), enabled);
    EnableWindow(GetDlgItem(owner, slider.label), enabled);
  }
  InvalidateRect(GetDlgItem(owner, boxPreview), nullptr, FALSE);
  InvalidateRect(desktopGroupsPreview, nullptr, FALSE);
}

static void previewBoxStyle(HWND owner, const desktop_groups::Appearance &previous) {
  if (boxStyle == previous) return;
  updateBoxStylePreview(owner);
  if (IsWindowVisible(owner)) {
    desktop_groups::previewAppearance(boxStyle);
    boxAppearancePreviewing = true;
  }
}

static void restoreBoxStylePreview() {
  if (!boxAppearancePreviewing) return;
  desktop_groups::restoreAppearance();
  boxAppearancePreviewing = false;
}

static void paintDesktopGroupsPreview(const DRAWITEMSTRUCT &item) {
  const auto dc = item.hDC;
  const auto rect = item.rcItem;
  const auto &colors = appearance->colors;
  const bool stylePage = item.CtlID == boxPreview;
  const bool glass = !colors.highContrast && (stylePage ? boxStyle.glass :
      SendMessageW(desktopGroupsGlassCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
  RECT panel{rect.left + scale(24), rect.top + scale(16),
             std::min(rect.right - scale(24), rect.left + scale(264)), rect.bottom - scale(16)};
  if (panel.right <= panel.left + scale(80)) return;

  const auto brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
  auto fill = [&](RECT area, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, brush);
  };
  auto wallpaper = [&](int tint, bool blurred = false) {
    constexpr int steps = 60;
    const int width = rect.right - rect.left;
    for (int i = 0; i < steps; ++i) {
      const auto color = i < steps / 2
          ? blend(RGB(57, 120, 182), RGB(79, 77, 154), i * 200 / steps)
          : blend(RGB(79, 77, 154), RGB(163, 99, 149), (i - steps / 2) * 200 / steps);
      fill({rect.left + width * i / steps, rect.top, rect.left + width * (i + 1) / steps, rect.bottom},
           blend(color, colors.field, tint));
    }
    if (!blurred) {
      const auto color = blend(RGB(72, 66, 138), colors.field, tint);
      const int height = rect.bottom - rect.top;
      fill({rect.left + width / 3, rect.top + height / 4, rect.left + width * 2 / 3, rect.top + height / 2}, color);
    }
  };
  if (colors.highContrast) fill(rect, colors.background);
  else wallpaper(0);
  if (!stylePage && SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) != BST_CHECKED) return;

  const int diameter = scale(boxStyle.radius * 2);
  if (boxStyle.shadow && boxStyle.shadowOpacity > 0 && !colors.highContrast) {
    // Use a reduced sample so the available preview area fits the full slider range.
    const int spread = std::max(1, scale((boxStyle.shadowBlur + 1) / 2));
    const int offset = scale((boxStyle.shadowOffset + 1) / 2);
    for (int ring = spread; ring >= 0; --ring) {
      auto shadow = panel;
      OffsetRect(&shadow, 0, offset);
      InflateRect(&shadow, ring, ring);
      if (const auto edge = CreateRoundRectRgn(shadow.left, shadow.top, shadow.right, shadow.bottom,
                                              diameter + ring * 2, diameter + ring * 2)) {
        const int falloff = spread - ring;
        const auto opacity = static_cast<int>(boxStyle.shadowOpacity) * falloff * falloff / (spread * spread);
        SetDCBrushColor(dc, blend(RGB(82, 84, 153), RGB(0, 0, 0), opacity));
        if (ring) FrameRgn(dc, edge, brush, 1, 1);
        else FillRgn(dc, edge, brush);
        DeleteObject(edge);
      }
    }
  }
  const auto shape = diameter ? CreateRoundRectRgn(panel.left, panel.top, panel.right, panel.bottom, diameter, diameter)
                              : CreateRectRgnIndirect(&panel);
  if (shape) {
    if (const int saved = SaveDC(dc)) {
      ExtSelectClipRgn(dc, shape, RGN_AND);
      if (glass && boxStyle.opacity < 100) wallpaper(boxStyle.opacity, true);
      else fill(panel, colors.field);
      RestoreDC(dc, saved);
    }
    SetDCBrushColor(dc, desktop_groups::boxOutlineColor(colors.field, colors));
    FrameRgn(dc, shape, brush, 1, 1);
    DeleteObject(shape);
  }
  const int pad = scale(12), headerHeight = std::min(scale(36), static_cast<int>(panel.bottom - panel.top) / 2);
  const auto previousFont = SelectObject(dc, font);
  RECT title{panel.left + pad, panel.top, panel.right - scale(37), panel.top + headerHeight};
  ui::text(dc, lang::text(L"文档"), title, colors.text);
  SelectObject(dc, previousFont);
  const int centerX = panel.right - scale(19), centerY = panel.top + headerHeight / 2, size = scale(5);
  const POINT arrow[]{{centerX - size, centerY + size / 2}, {centerX, centerY - size / 2},
                      {centerX + size, centerY + size / 2}};
  if (const auto pen = CreatePen(PS_SOLID, std::max(1, scale(1)), colors.muted)) {
    const auto previous = SelectObject(dc, pen);
    Polyline(dc, arrow, ARRAYSIZE(arrow));
    SelectObject(dc, previous);
    DeleteObject(pen);
  }
  SHSTOCKICONINFO stock{sizeof(stock)};
  if (FAILED(SHGetStockIconInfo(SIID_FOLDER, SHGSI_ICON | SHGSI_LARGEICON, &stock)) || !stock.hIcon) return;
  const int y = panel.top + headerHeight + scale(9);
  const int iconSize = std::min(scale(27), static_cast<int>(panel.bottom) - y - scale(6));
  for (int i = 0; i < 3; ++i) {
    const int x = panel.left + scale(16 + i * 49);
    if (iconSize <= 0 || x + iconSize >= panel.right) break;
    DrawIconEx(dc, x, y, stock.hIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
  }
  DestroyIcon(stock.hIcon);
}

static void copyDiagnostics(HWND owner) {
  const auto text = diagnostics();
  auto memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
  if (!memory)
    fn::fail(L"无法复制诊断信息");
  auto destination = GlobalLock(memory);
  if (!destination) {
    GlobalFree(memory);
    fn::fail(L"无法复制诊断信息");
  }
  memcpy(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
  GlobalUnlock(memory);
  if (!OpenClipboard(owner)) {
    GlobalFree(memory);
    fn::fail(L"剪贴板暂时不可用，请重试");
  }
  const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
  CloseClipboard();
  if (!copied) {
    GlobalFree(memory);
    fn::fail(L"无法复制诊断信息");
  }
  MessageBoxW(owner, lang::text(L"诊断信息已复制，可粘贴给开发者排查。"), L"FolderNote", MB_OK | MB_ICONINFORMATION);
}
static void layoutBody() {
  if (!body) return;
  RECT client{}; GetClientRect(body, &client);
  scrollPosition = std::clamp(scrollPosition, 0, std::max(0, contentHeight() - int(client.bottom)));
  settingsScroll.configure(contentHeight(), client.bottom, scrollPosition, dpi, scale(16));
  if (activePage != SettingsPage::Features) ShowWindow(settingsScroll.window(), SW_HIDE);
  constexpr UINT moveFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS;
  auto batch = BeginDeferWindowPos(static_cast<int>(placements.size()));
  for (const auto &item : placements) {
    const int left = scale(item.x);
    const bool mainSwitch = item.window == notesCheck || item.window == navigationCheck ||
        item.window == quickCheck || item.window == tabsCheck || item.window == desktopGroupsCheck;
    const int right = client.right - scale(mainSwitch ? 40 : 86);
    int width = item.width < 0 ? right - left : std::min(scale(item.width), right - left);
    if (item.window == choice) {
      if (auto dc = GetDC(choice)) {
        auto previous = SelectObject(dc, font);
        int longest = 0;
        const auto count = SendMessageW(choice, CB_GETCOUNT, 0, 0);
        for (LRESULT index = 0; index < count; ++index) {
          const auto length = SendMessageW(choice, CB_GETLBTEXTLEN, index, 0);
          if (length < 0) continue;
          std::wstring label(size_t(length) + 1, L'\0');
          const auto copied = SendMessageW(choice, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(label.data()));
          SIZE extent{};
          if (copied >= 0 && GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(copied), &extent))
            longest = std::max(longest, int(extent.cx));
        }
        width = std::min(width, longest + scale(48));
        SelectObject(dc, previous);
        ReleaseDC(choice, dc);
      }
    }
    if (item.window == dialogCheck || item.window == preferNewTabCheck) {
      const int length = GetWindowTextLengthW(item.window);
      std::wstring label(size_t(length) + 1, L'\0');
      GetWindowTextW(item.window, label.data(), length + 1);
      if (auto dc = GetDC(item.window)) {
        auto previous = SelectObject(dc, font);
        SIZE extent{};
        if (GetTextExtentPoint32W(dc, label.c_str(), length, &extent))
          width = std::min(width, int(extent.cx) + scale(34));
        SelectObject(dc, previous);
        ReleaseDC(item.window, dc);
      }
    }
    const int top = scale(item.y) - scrollPosition;
    if (batch) batch = DeferWindowPos(batch, item.window, nullptr, left, top,
        std::max(0, width), scale(item.height), moveFlags);
    else SetWindowPos(item.window, nullptr, left, top, std::max(0, width), scale(item.height), moveFlags);
  }
  if (batch) EndDeferWindowPos(batch);
  RedrawWindow(body, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
static void layoutSettings() {
  RECT client{}; GetClientRect(settingsWindow, &client);
  int width = client.right, height = client.bottom;
  const int sidebar = settingsSidebarWidth;
  MoveWindow(GetDlgItem(settingsWindow, 35), scale(20), scale(20), scale(172), scale(32), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, 36), scale(20), scale(57), scale(172), scale(22), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, 30), scale(sidebar + 24), scale(27), width - scale(sidebar + 254), scale(32), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, 31), scale(20), height - scale(39), scale(sidebar - 79), scale(22), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, updateButton), scale(sidebar - 52), height - scale(44), scale(32), scale(32), TRUE);
  for (size_t i = 0; i < pageButtons.size(); ++i)
    MoveWindow(GetDlgItem(settingsWindow, pageButtons[i]), scale(12), scale(95 + int(i) * 44), scale(sidebar - 24), scale(38), TRUE);
  auto preference = [&](int id, int x, int y, int w, int h) {
    auto control = GetDlgItem(settingsWindow, id);
    MoveWindow(control, scale(sidebar + 24 + x), scale(y), scale(w), scale(h), TRUE);
    ShowWindow(control, activePage == SettingsPage::Preferences ? SW_SHOWNA : SW_HIDE);
  };
  preference(46, 0, 84, 210, 22);
  preference(languageChoice, 0, 110, 210, 130);
  preference(languageExport, 0, 150, 174, 32);
  preference(languageImport, 184, 150, 174, 32);
  preference(32, 300, 84, 210, 22);
  preference(17, 300, 110, 210, 130);
  const int preferencesWidth = std::max(0, MulDiv(width, 96, dpi) - sidebar - 48);
  preference(automaticUpdateCheck, 0, 208, std::min(210, preferencesWidth), 26);
  preference(automaticUpdateDescription, 0, 240, preferencesWidth, 42);
  constexpr int statusTop = 292;
  const int statusHeight = std::clamp(MulDiv(height, 96, dpi) - 72 - statusTop, 0, 62);
  preference(updateStatusLabel, 0, statusTop, preferencesWidth, statusHeight);
  const int findColumn = (preferencesWidth - 24) / 2;
  auto findControl = [&](int id, int x, int y, int w, int h) {
    auto control = GetDlgItem(settingsWindow, id);
    MoveWindow(control, scale(sidebar + 24 + x), scale(y), scale(w), scale(h), TRUE);
    ShowWindow(control, activePage == SettingsPage::QuickFind ? SW_SHOWNA : SW_HIDE);
  };
  findControl(findDescription, 0, 80, preferencesWidth, 36);
  findControl(findScope, 0, 122, preferencesWidth, 20);
  findControl(findBoxes, 0, 146, findColumn, 26);
  findControl(findLists, findColumn + 24, 146, findColumn, 26);
  findControl(findExplorer, 0, 176, findColumn, 26);
  findControl(findDialogs, findColumn + 24, 176, findColumn, 26);
  for (const auto ids : {std::pair{findFolderLabel, findFolderAction}, std::pair{findFileLabel, findFileAction},
                         std::pair{findPositionLabel, findPosition}, std::pair{findRowsLabel, findRows}}) {
    const bool second = ids.first == findFileLabel || ids.first == findRowsLabel;
    const int y = ids.first < findPositionLabel ? 212 : 282;
    findControl(ids.first, second ? findColumn + 24 : 0, y, findColumn, 20);
    findControl(ids.second, second ? findColumn + 24 : 0, y + 24, findColumn, 200);
  }
  findControl(findIcons, 0, 350, preferencesWidth, 26);
  findControl(findHint, 0, 382, preferencesWidth, 32);
  const int boxLeft = scale(sidebar + 24);
  const int boxWidth = std::max(0, width - scale(sidebar + 48));
  const int boxGap = scale(24);
  const int boxColumn = std::max(0, (boxWidth - boxGap) / 2);
  auto boxControl = [&](int id, int x, int y, int w, int h) {
    auto control = GetDlgItem(settingsWindow, id);
    MoveWindow(control, boxLeft + x, scale(y), w, h, TRUE);
    ShowWindow(control, activePage == SettingsPage::BoxAppearance ? SW_SHOWNA : SW_HIDE);
  };
  boxControl(boxGlass, 0, 81, std::min(boxColumn, scale(224)), scale(26));
  boxControl(boxShadow, boxColumn + boxGap, 81, std::min(boxColumn, scale(224)), scale(26));
  for (size_t index = 0; index < std::size(boxSliders); ++index) {
    const auto &slider = boxSliders[index];
    const int x = index == 0 ? 0 : boxColumn + boxGap;
    const int y = index == 0 ? 116 : 116 + 45 * static_cast<int>(index - 1);
    boxControl(slider.label, x, y, boxColumn, scale(19));
    boxControl(slider.control, x, y + 19, boxColumn, scale(23));
  }
  boxControl(boxPreviewLabel, 0, 257, boxWidth, scale(20));
  const int previewHeight = std::clamp(height - scale(354), scale(92), scale(132));
  boxControl(boxPreview, 0, 278, boxWidth, previewHeight);
  int bodyHeight = std::max(scale(40), height - scale(166));
  bool overflow = bodyHeight < contentHeight();
  const int contentLeft = scale(sidebar + 5);
  MoveWindow(body, contentLeft, scale(88), width - contentLeft - (overflow ? scale(20) : 0), bodyHeight, TRUE);
  MoveWindow(managerPage, contentLeft, scale(88), width - contentLeft, bodyHeight, TRUE);
  MoveWindow(contextPage, contentLeft, 0, width - contentLeft, height, TRUE);
  MoveWindow(entriesPage, contentLeft, scale(88), width - contentLeft, std::max(0, height - scale(88)), TRUE);
  settingsScroll.move(width - scale(18), scale(88), scale(12), bodyHeight);
  MoveWindow(GetDlgItem(settingsWindow, 33), scale(sidebar + 24), height - scale(70), width - scale(sidebar + 48), scale(18), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, 13), scale(sidebar + 24), height - scale(44), scale(140), scale(32), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, IDOK), width - scale(212), height - scale(44), scale(88), scale(32), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, IDCANCEL), width - scale(112), height - scale(44), scale(88), scale(32), TRUE);
  for (int id : {33, 13, IDOK, IDCANCEL})
    ShowWindow(GetDlgItem(settingsWindow, id),
        activePage == SettingsPage::ContextMenus || activePage == SettingsPage::ExplorerEntries ||
        (activePage == SettingsPage::QuickFind && id != IDCANCEL) ? SW_HIDE : SW_SHOWNA);
  layoutBody();
  ShowWindow(settingsScroll.window(), activePage == SettingsPage::Features && overflow ? SW_SHOWNA : SW_HIDE);
}
static void showPage(SettingsPage page) {
  activePage = page;
  const std::array pages{body, managerPage, contextPage, entriesPage, HWND{}, HWND{}, HWND{}};
  const auto target = pages[size_t(page)];
  for (auto panel : pages)
    if (panel && panel != target) ShowWindow(panel, SW_HIDE);
  ShowWindow(GetDlgItem(settingsWindow, 30), page == SettingsPage::ContextMenus ? SW_HIDE : SW_SHOWNA);
  SetWindowTextW(GetDlgItem(settingsWindow, 30), pageTitle(size_t(page)));
  SetWindowTextW(GetDlgItem(settingsWindow, IDCANCEL), lang::text(page == SettingsPage::QuickFind ? L"关闭" : L"取消"));
  for (int id : pageButtons)
    RedrawWindow(GetDlgItem(settingsWindow, id), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  layoutSettings();
  if (target) ShowWindow(target, SW_SHOWNA);
  if (page == SettingsPage::QuickFolders) quick::refreshManagerPage(managerPage);
  if (page == SettingsPage::ExplorerEntries) explorer_entries_page::refresh(entriesPage);
}
static LRESULT CALLBACK navigationChoiceProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_MOUSEWHEEL) {
    SendMessageW(GetParent(h), message, w, l);
    return 0;
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(h, navigationChoiceProc, id);
  return DefSubclassProc(h, message, w, l);
}
static LRESULT CALLBACK sidebarProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END)) {
    const int count = int(pageButtons.size());
    const int current = int(std::find(pageButtons.begin(), pageButtons.end(), GetDlgCtrlID(h)) - pageButtons.begin());
    const int next = w == VK_HOME ? 0 : w == VK_END ? count - 1 : (current + (w == VK_UP ? count - 1 : 1)) % count;
    showPage(SettingsPage(next)); SetFocus(GetDlgItem(settingsWindow, pageButtons[next])); return 0;
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(h, sidebarProc, id);
  return DefSubclassProc(h, message, w, l);
}
static LRESULT CALLBACK contentProc(HWND h, UINT message, WPARAM w, LPARAM l) {
  LRESULT result = 0;
  if (appearance && appearance->handle(h, message, w, l, result)) return result;
  switch (message) {
  case WM_DRAWITEM: {
    const auto &item = *reinterpret_cast<const DRAWITEMSTRUCT *>(l);
    if (item.CtlID == desktopGroupsPreviewId) {
      paintDesktopGroupsPreview(item);
      return TRUE;
    }
    break;
  }
  case WM_PAINT: {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(h, &paint); RECT client{}; GetClientRect(h, &client);
    FillRect(dc, &client, appearance->background);
    SetDCBrushColor(dc, appearance->colors.border);
    for (int separator : {94, 228, 445, 445 + desktopGroupsHeight}) {
      if (separator == 445 + desktopGroupsHeight && !tabsSupported()) continue;
      const int y = scale(separator) - scrollPosition;
      RECT line{scale(20), y, client.right - scale(20), y + 1};
      FillRect(dc, &line, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    EndPaint(h, &paint); return 0;
  }
  case WM_COMMAND: {
    HWND control = reinterpret_cast<HWND>(l);
    if (control && GetFocus() == control) {
      auto found = std::find_if(placements.begin(), placements.end(), [&](const auto &p) { return p.window == control; });
      if (found != placements.end()) {
        RECT client{}; GetClientRect(h, &client);
        if (scale(found->y) - scrollPosition < 0) scrollPosition = scale(found->y) - scale(8);
        else if (scale(found->y + 32) - scrollPosition > client.bottom) scrollPosition = scale(found->y + 40) - client.bottom;
        layoutBody();
      }
    }
    return SendMessageW(GetParent(h), message, w, l);
  }
  case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = scale(26); return TRUE;
  case WM_VSCROLL: {
    settingsScroll.command(w); return 0;
  }
  case WM_MOUSEWHEEL: if (activePage == SettingsPage::Features) settingsScroll.wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0;
  }
  return DefWindowProcW(h, message, w, l);
}
static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == selectPageMessage()) {
    showPage(w < pageButtons.size() ? SettingsPage(w) : SettingsPage::Features); return 0;
  }
  if (m == ui::themeChangedMessage() && appearance) {
    appearance->reset(ui::configuredTheme());
    contextmenu::setTheme(contextPage, unsigned(appearance->mode));
    SendMessageW(themeChoice, CB_SETCURSEL, DWORD(appearance->mode), 0); appearance->apply(h); quick::setManagerTheme(managerPage, appearance->mode);
    SendMessageW(entriesPage, WM_THEMECHANGED, 0, 0); return 0;
  }
  if (m == WM_DRAWITEM && appearance) {
    auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
    if (item->CtlID == boxPreview) {
      paintDesktopGroupsPreview(*item);
      return TRUE;
    }
    if (item->CtlType == ODT_BUTTON && item->CtlID == updateButton) {
      const auto &p = appearance->colors;
      const bool selected = (item->itemState & ODS_SELECTED) != 0;
      const bool disabled = (item->itemState & ODS_DISABLED) != 0;
      FillRect(item->hDC, &item->rcItem, appearance->background);
      ui::rounded(item->hDC, item->rcItem, selected ? p.pressed : p.field, p.border, scale(5));
      auto symbols = ui::makeSymbolFont(dpi, 16);
      auto old = SelectObject(item->hDC, symbols);
      const auto color = disabled ? p.muted : availableUpdate ? p.accent : p.text;
      ui::text(item->hDC, availableUpdate ? L"\xE896" : L"\xE72C", item->rcItem, color,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(item->hDC, old); DeleteObject(symbols);
      return TRUE;
    }
    auto button = std::find(pageButtons.begin(), pageButtons.end(), int(item->CtlID));
    if (item->CtlType == ODT_BUTTON && button != pageButtons.end()) {
      const auto &p = appearance->colors;
      const auto page = size_t(button - pageButtons.begin());
      bool selected = page == size_t(activePage);
      FillRect(item->hDC, &item->rcItem, appearance->background);
      RECT rect = item->rcItem; InflateRect(&rect, -scale(2), -scale(2));
      const auto fill = selected ? p.accent : (item->itemState & (ODS_SELECTED | ODS_HOTLIGHT | ODS_FOCUS)) ? p.hover : p.background;
      ui::rounded(item->hDC, rect, fill, fill, scale(6));
      auto old = SelectObject(item->hDC, font);
      ui::text(item->hDC, pageTitle(page), rect, selected ? p.onAccent : p.text,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      SelectObject(item->hDC, old);
      return TRUE;
    }
  }
  LRESULT result = 0;
  if (appearance && appearance->handle(h, m, w, l, result)) return result;
  switch (m) {
  case WM_SETCURSOR:
    if (LOWORD(l) == HTCLIENT) {
      POINT point{};
      GetCursorPos(&point); ScreenToClient(h, &point);
      if (abs(point.x - scale(settingsSidebarWidth)) <= scale(4)) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE;
      }
    }
    break;
  case WM_LBUTTONDOWN:
    if (abs(GET_X_LPARAM(l) - scale(settingsSidebarWidth)) <= scale(4)) {
      draggingSettingsSidebar = true; SetCapture(h); return 0;
    }
    break;
  case WM_MOUSEMOVE:
    if (draggingSettingsSidebar) {
      const int width = std::clamp(MulDiv(GET_X_LPARAM(l), 96, dpi), 192, 244);
      if (width != settingsSidebarWidth) {
        settingsSidebarWidth = width;
        layoutSettings(); InvalidateRect(h, nullptr, TRUE);
      }
      return 0;
    }
    break;
  case WM_LBUTTONUP:
    if (draggingSettingsSidebar) { ReleaseCapture(); return 0; }
    break;
  case WM_CAPTURECHANGED: draggingSettingsSidebar = false; break;
  case WM_PAINT: {
    PAINTSTRUCT paint{};
    auto dc = BeginPaint(h, &paint);
    RECT client{};
    GetClientRect(h, &client);
    FillRect(dc, &client, appearance->background);
    auto sidebar = client;
    sidebar.right = scale(settingsSidebarWidth);
    RECT separator{sidebar.right - 1, client.top, sidebar.right, client.bottom};
    SetDCBrushColor(dc, appearance->colors.border);
    FillRect(dc, &separator, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    EndPaint(h, &paint);
    return 0;
  }
  case WM_CREATE: {
    SetPropW(h, app::settingsWindowTag, reinterpret_cast<HANDLE>(1));
    settingsWindow = h; dpi = GetDpiForWindow(h); scrollPosition = 0; placements.clear();
    font = ui::makeFont(dpi); headingFont = ui::makeFont(dpi, 23, FW_SEMIBOLD);
    auto add = [&](HWND parent, LPCWSTR cls, LPCWSTR text, DWORD style, int id) {
      HWND control = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
          parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); return control;
    };
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
    boxStyle = desktop_groups::configuredAppearance();
    if (languageDraft) boxStyle = languageDraft->boxStyle;
    constexpr DWORD appearanceCheck = BS_AUTOCHECKBOX | WS_TABSTOP;
    const auto glassLabel = std::wstring(lang::text(L"毛玻璃效果")) + L" (Acrylic)";
    auto glassBox = add(h, L"BUTTON", glassLabel.c_str(), appearanceCheck, boxGlass);
    auto shadowBox = add(h, L"BUTTON", lang::text(L"投影"), appearanceCheck, boxShadow);
    ui::toggleSwitch(glassBox);
    ui::toggleSwitch(shadowBox);
    SendMessageW(glassBox, BM_SETCHECK, boxStyle.glass ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(shadowBox, BM_SETCHECK, boxStyle.shadow ? BST_CHECKED : BST_UNCHECKED, 0);
    for (const auto &spec : boxSliders) {
      add(h, L"STATIC", L"", 0, spec.label);
      const auto slider = add(h, TRACKBAR_CLASSW, lang::text(spec.name), TBS_NOTICKS | WS_TABSTOP, spec.control);
      SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(spec.min, spec.max));
      SendMessageW(slider, TBM_SETPOS, TRUE, boxStyle.*spec.value);
    }
    ui::muted(add(h, L"STATIC", lang::text(L"预览"), 0, boxPreviewLabel));
    add(h, L"STATIC", lang::text(L"盒子样式预览"), SS_OWNERDRAW, boxPreview);
    HWND logo = add(h, L"STATIC", L"FolderNote", 0, 35);
    SendMessageW(logo, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    auto subtitle = add(h, L"STATIC", lang::text(L"系统工具"), 0, 36);
    ui::muted(subtitle);
    for (size_t page = 0; page < pageButtons.size(); ++page) {
      auto button = add(h, L"BUTTON", pageTitle(page), BS_OWNERDRAW | WS_TABSTOP, pageButtons[page]);
      SetWindowSubclass(button, sidebarProc, 1, 0);
    }
    HWND title = add(h, L"STATIC", lang::text(L"功能设置"), 0, 30);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    const auto version = lang::format(L"版本 {0} · x64", {app::version});
    auto versionControl = add(h, L"STATIC", version.c_str(), SS_LEFT | SS_NOPREFIX, 31);
    ui::muted(versionControl);
    auto update = add(h, L"BUTTON", lang::text(L"检查更新"), WS_TABSTOP | BS_OWNERDRAW, updateButton);
    updateTip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, h, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (updateTip) {
      TOOLINFOW tool{sizeof(tool)};
      tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
      tool.hwnd = h;
      tool.uId = reinterpret_cast<UINT_PTR>(update);
      tool.lpszText = const_cast<wchar_t *>(lang::text(L"检查更新"));
      SendMessageW(updateTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    ui::muted(add(h, L"STATIC", lang::text(L"外观主题"), 0, 32));
    const auto automaticUpdate = add(h, L"BUTTON", lang::text(L"自动更新"), BS_AUTOCHECKBOX | WS_TABSTOP,
                                     automaticUpdateCheck);
    SendMessageW(automaticUpdate, BM_SETCHECK, automaticUpdatesEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    ui::muted(add(h, L"STATIC", lang::text(L"开启后，打开设置时每天最多检查一次。通用版会自动安装无需管理员授权的更新。"),
                  SS_NOPREFIX, automaticUpdateDescription));
    ui::muted(add(h, L"STATIC", L"", SS_NOPREFIX, updateStatusLabel));
    updateControls(h);
    constexpr DWORD comboStyle = CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP;
    ui::muted(add(h, L"STATIC", lang::text(L"输入名称或中文首字母显示候选菜单。更改立即保存，下次搜索生效。"), SS_NOPREFIX, findDescription));
    ui::muted(add(h, L"STATIC", lang::text(L"使用范围"), SS_NOPREFIX, findScope));
    for (const auto &toggle : findToggles)
      add(h, L"BUTTON", lang::text(toggle.label), BS_AUTOCHECKBOX | WS_TABSTOP, toggle.id);
    for (const auto field : {std::pair{findFolderLabel, L"选择文件夹时"}, std::pair{findFileLabel, L"选择文件时"},
                             std::pair{findPositionLabel, L"弹窗位置"}, std::pair{findRowsLabel, L"可见候选数量"}})
      ui::muted(add(h, L"STATIC", lang::text(field.second), SS_NOPREFIX, field.first));
    for (int id : {findFolderAction, findFileAction, findPosition, findRows}) {
      auto combo = add(h, L"COMBOBOX", L"", comboStyle, id);
      if (id == findRows) {
        for (int rows = 5; rows <= 12; ++rows) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(rows).c_str()));
      } else {
        const auto labels = id == findPosition ? std::array{L"鼠标位置", L"当前列表上方"} : std::array{L"仅定位选中", L"直接打开"};
        for (auto label : labels) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lang::text(label)));
      }
      SendMessageW(combo, CB_SETITEMHEIGHT, WPARAM(-1), scale(26));
    }
    ui::muted(add(h, L"STATIC", lang::text(L"系统文件窗口中的文件始终只选中；菜单命令仅定位，不自动执行。"), SS_NOPREFIX, findHint));
    refreshFindSettings(h);
    ui::muted(add(h, L"STATIC", lang::text(L"界面语言"), 0, 46));
    HWND language = add(h, L"COMBOBOX", L"", comboStyle, languageChoice);
    SendMessageW(language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"简体中文"));
    SendMessageW(language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    auto languageSettings = lang::settings();
    if (!languageSettings.customName.empty())
      SendMessageW(language, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(languageSettings.customName.c_str()));
    auto selectedLanguage = languageSettings.choice;
    if (selectedLanguage == lang::Choice::Custom && languageSettings.customName.empty()) selectedLanguage = lang::Choice::English;
    SendMessageW(language, CB_SETCURSEL, static_cast<unsigned>(selectedLanguage), 0);
    SendMessageW(language, CB_SETITEMHEIGHT, WPARAM(-1), scale(26));
    add(h, L"BUTTON", lang::text(L"导出语言模板"), WS_TABSTOP, languageExport);
    add(h, L"BUTTON", lang::text(L"导入语言文件"), WS_TABSTOP, languageImport);
    themeChoice = add(h, L"COMBOBOX", L"", comboStyle, 17);
    for (auto titleText : {lang::text(L"跟随系统"), lang::text(L"浅色"), lang::text(L"深色")}) SendMessageW(themeChoice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(titleText));
    SendMessageW(themeChoice, CB_SETCURSEL, DWORD(appearance->mode), 0);
    SendMessageW(themeChoice, CB_SETITEMHEIGHT, WPARAM(-1), scale(26));
    body = CreateWindowExW(WS_EX_CONTROLPARENT, L"FolderNote.Settings.Content", lang::text(L"功能设置"),
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, h, nullptr, GetModuleHandleW(nullptr), nullptr);
    settingsScroll.create(h, 34, *appearance, [](int position) { scrollPosition = position; layoutBody(); });
    managerPage = quick::createManagerPage(GetModuleHandleW(nullptr), h, appearance->mode);
    contextPage = contextmenu::createPage(GetModuleHandleW(nullptr), h, [](int operation, bool hidden) {
      if (!hidden && !notesEnabled()) {
        const auto action = configuredAction();
        const auto preferred = preferredAction();
        for (int op = 0; op < 3; ++op) setNoteCommandHidden(op, op != operation);
        saveSettings(true, action, preferred);
        SendMessageW(notesCheck, BM_SETCHECK, BST_CHECKED, 0);
      } else setNoteCommandHidden(operation, hidden);
    });
    entriesPage = explorer_entries_page::create(h, *appearance, font);
    if (!managerPage || !contextPage || !entriesPage) return -1;
    auto field = [&](LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int width, int height, int id, bool quiet = false) {
      HWND c = add(body, cls, text, style, id); placements.push_back({c, x, y, width, height});
      if (quiet) ui::muted(c);
      return c;
    };
    constexpr DWORD checkStyle = BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP;
    notesCheck = field(L"BUTTON", lang::text(L"文件夹备注"), checkStyle, 40, 14, -40, 26, 11);
    SendMessageW(notesCheck, BM_SETCHECK, notesEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    field(L"STATIC", lang::text(L"开启后在文件夹右键菜单中添加、编辑和修复备注。\n关闭只隐藏入口，已有备注会保留。"), 0, 86, 45, -40, 38, 0, true);
    navigationCheck = field(L"BUTTON", lang::text(L"双击空白处导航"), checkStyle, 40, 112, -40, 26, 12);
    const auto action = configuredAction();
    SendMessageW(navigationCheck, BM_SETCHECK, action != Action::Off ? BST_CHECKED : BST_UNCHECKED, 0);
    choice = field(L"COMBOBOX", L"", comboStyle, 86, 146, -40, 180, 10);
    SetWindowSubclass(choice, navigationChoiceProc, 1, 0);
    for (auto titleText : {lang::text(L"返回上一级文件夹"), lang::text(L"后退到上一次浏览的位置")}) SendMessageW(choice, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(titleText));
    SendMessageW(choice, CB_SETCURSEL, preferredAction() == Action::Back ? 1 : 0, 0);
    SendMessageW(choice, CB_SETITEMHEIGHT, WPARAM(-1), scale(26));
    EnableWindow(choice, action != Action::Off);
    field(L"STATIC", lang::text(L"在资源管理器文件列表的空白处双击。"), 0, 86, 190, -40, 22, 0, true);
    quickCheck = field(L"BUTTON", lang::text(L"中键快速定位"), checkStyle, 40, 246, -40, 26, 14);
    SendMessageW(quickCheck, BM_SETCHECK, quickEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    dialogCheck = field(L"BUTTON", lang::text(L"同时支持文件与文件夹选择对话框"), checkStyle, 86, 278, -40, 26, 15);
    SendMessageW(dialogCheck, BM_SETCHECK, dialogQuickEnabled() ? BST_CHECKED : BST_UNCHECKED, 0); EnableWindow(dialogCheck, quickEnabled());
    field(L"STATIC", lang::text(L"桌面和资源管理器中均可使用，悬停展开子目录。\n启用后运行后台组件，快速访问常用和最近打开的文件夹。"), 0, 86, 309, -40, 38, 0, true);
    field(L"STATIC", lang::text(L"选中项目后按中键，点击“复制”或“移动”，\n再选择目标文件夹；在空白处按中键也可使用。"), 0, 86, 352, -40, 38, 0, true);
    field(L"BUTTON", lang::text(L"置顶与历史 →"), WS_TABSTOP, 86, 402, 220, 28, 16);
    desktopGroupsCheck = field(L"BUTTON", lang::text(L"桌面盒子"), checkStyle, 40, 462, -40, 26, desktopGroupsToggle);
    SendMessageW(desktopGroupsCheck, BM_SETCHECK, desktop_groups::enabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    field(L"STATIC", lang::text(L"由 Windows 管理桌面图标。\n拖入盒子的桌面文件和文件夹会移到隐藏的 .desktop 目录。"), 0, 86, 494, -40, 38, 0, true);
    desktopGroupsGlassCheck = field(L"BUTTON", glassLabel.c_str(), checkStyle, 86, 538, 185, 26, desktopGroupsGlassToggle);
    SendMessageW(desktopGroupsGlassCheck, BM_SETCHECK, configuredDesktopGroupsGlass() ? BST_CHECKED : BST_UNCHECKED, 0);
    desktopGroupsManager = field(L"BUTTON", lang::text(L"管理桌面盒子"), WS_TABSTOP | BS_NOTIFY,
        86, 574, 185, 28, desktopGroupsManage);
    desktopGroupsPreview = field(L"STATIC", lang::text(L"桌面盒子效果预览"), SS_OWNERDRAW,
        285, desktopGroupsPreviewTop, 380, desktopGroupsPreviewHeight, desktopGroupsPreviewId);
    field(L"BUTTON", lang::text(L"盒子样式 →"), WS_TABSTOP, 86, 612, 185, 28, boxStyleShortcut);
    updateBoxStylePreview(h);
    tabsCheck = preferNewTabCheck = nullptr;
    if (tabsSupported()) {
      tabsCheck = field(L"BUTTON", lang::text(L"资源管理器多标签（Windows 10）"), checkStyle, 40, 462 + desktopGroupsHeight, -40, 26, 18);
      SendMessageW(tabsCheck, BM_SETCHECK, tabsEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
      field(L"STATIC", lang::text(L"悬停标签：滚轮切换，中键关闭；右键可新建和设置。\n保存后显示或隐藏标签栏；升级后请重新打开文件夹窗口。"), 0, 86, 494 + desktopGroupsHeight, -40, 40, 0, true);
      preferNewTabCheck = field(L"BUTTON", lang::text(L"始终在新标签中打开文件夹"), checkStyle, 86, 548 + desktopGroupsHeight, -40, 26, 21);
      SendMessageW(preferNewTabCheck, BM_SETCHECK, preferNewTabEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
      EnableWindow(preferNewTabCheck, tabsEnabled());
      field(L"STATIC", lang::text(L"首个窗口正常打开；桌面或其他软件另开文件夹时\n优先使用新标签；标签右键仍可开新窗口。"), 0, 114, 580 + desktopGroupsHeight, -40, 40, 0, true);
    }
    for (auto toggle : {notesCheck, navigationCheck, quickCheck, tabsCheck, desktopGroupsCheck})
      if (toggle) ui::toggleSwitch(toggle);
    ui::muted(add(h, L"STATIC", lang::text(L"功能设置保存后应用 · 置顶与历史修改立即保存"), 0, 33));
    add(h, L"BUTTON", lang::text(L"复制诊断信息"), WS_TABSTOP, 13);
    add(h, L"BUTTON", lang::text(L"保存"), WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    add(h, L"BUTTON", lang::text(L"取消"), WS_TABSTOP, IDCANCEL);
    if (languageDraft) {
      std::array checks{notesCheck, navigationCheck, quickCheck, dialogCheck, tabsCheck, preferNewTabCheck,
                        desktopGroupsCheck, desktopGroupsGlassCheck};
      for (size_t i = 0; i < checks.size(); ++i)
        if (checks[i]) SendMessageW(checks[i], BM_SETCHECK, languageDraft->toggles[i], 0);
      SendMessageW(choice, CB_SETCURSEL, languageDraft->action, 0);
      appearance->reset(languageDraft->theme);
      SendMessageW(themeChoice, CB_SETCURSEL, DWORD(appearance->mode), 0);
      EnableWindow(choice, languageDraft->toggles[1] == BST_CHECKED);
      EnableWindow(dialogCheck, languageDraft->toggles[2] == BST_CHECKED);
      if (preferNewTabCheck) EnableWindow(preferNewTabCheck, languageDraft->toggles[4] == BST_CHECKED);
      languageDraft.reset();
    }
    updateDesktopGroupsManager();
    appearance->apply(h); quick::setManagerTheme(managerPage, appearance->mode); showPage(activePage); return 0;
  }
  case WM_COMMAND:
    if (installingManually() && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) return 0;
    if ((LOWORD(w) == languageChoice && HIWORD(w) == CBN_SELCHANGE) ||
        ((LOWORD(w) == languageExport || LOWORD(w) == languageImport) && HIWORD(w) == BN_CLICKED)) {
      const int id = LOWORD(w);
      try {
        if (id == languageChoice) {
          auto selection = SendDlgItemMessageW(h, languageChoice, CB_GETCURSEL, 0, 0);
          if (selection < 0 || selection > 2 || static_cast<lang::Choice>(selection) == lang::settings().choice) return 0;
          lang::select(static_cast<lang::Choice>(selection));
        } else {
          auto path = languagePath(h, id == languageImport);
          if (path.empty()) return 0;
          if (id == languageExport) { lang::exportFile(path); return 0; }
          lang::importFile(path);
        }
        refreshLanguageMenus();
        SettingsDraft draft{{}, SendMessageW(choice, CB_GETCURSEL, 0, 0), appearance->mode, boxStyle};
        std::array checks{notesCheck, navigationCheck, quickCheck, dialogCheck, tabsCheck, preferNewTabCheck,
                          desktopGroupsCheck, desktopGroupsGlassCheck};
        for (size_t i = 0; i < checks.size(); ++i)
          if (checks[i]) draft.toggles[i] = SendMessageW(checks[i], BM_GETCHECK, 0, 0);
        languageDraft = draft;
        reopenForLanguage = true;
        DestroyWindow(h);
      } catch (...) {
        SendDlgItemMessageW(h, languageChoice, CB_SETCURSEL, static_cast<unsigned>(lang::settings().choice), 0);
        MessageBoxW(h, id == languageImport ? lang::text(L"无法导入语言文件。请使用有效的 FolderNote XML 语言模板。") :
            lang::text(L"无法保存语言设置或模板。"), L"FolderNote", MB_OK | MB_ICONERROR);
      }
      return 0;
    }
    if (LOWORD(w) == IDOK) {
      try {
        const auto selection = SendMessageW(choice, CB_GETCURSEL, 0, 0);
        if (selection != 0 && selection != 1) throw fn::Error{lang::text(L"请选择有效的双击操作。")};
        const auto preferred = selection == 1 ? Action::Back : Action::Parent;
        const auto action = SendMessageW(navigationCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? preferred : Action::Off;
        applySettings(SendMessageW(notesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED, action, preferred,
            SendMessageW(quickCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
            SendMessageW(dialogCheck, BM_GETCHECK, 0, 0) == BST_CHECKED, appearance->mode,
            tabsCheck ? SendMessageW(tabsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : tabsEnabled(),
            preferNewTabCheck ? SendMessageW(preferNewTabCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : preferNewTabEnabled(),
            true, DesktopGroupsSettings{
              SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED,
              boxStyle.glass, boxStyle.radius, boxStyle.opacity, boxStyle.shadow,
              boxStyle.shadowOpacity, boxStyle.shadowBlur, boxStyle.shadowOffset});
        restoreBoxStylePreview();
        updateDesktopGroupsManager();
        const bool desktopGroupsSynced = desktop_groups::sync();
        ui::notifyThemeChanged(); reload(); auto tabsStatus = syncTabs(); quick::syncHelper(quick::executable());
        if (!desktopGroupsSynced)
          throw fn::Error{lang::text(L"设置已保存，但无法应用桌面盒子设置。请再次点击保存。")};
        if (tabsSupported() && tabsEnabled() && tabsStatus.windows && tabsStatus.visible == 0) {
          MessageBoxW(h, lang::text(L"设置已保存，已打开的文件夹窗口尚未显示标签栏。\n请关闭后重新打开文件夹窗口。标签栏由这里的开关控制，无需在资源管理器菜单中勾选。\n仍未显示时，可复制诊断信息查看加载状态。"), lang::text(L"FolderNote 多标签"), MB_OK | MB_ICONINFORMATION);
        } else DestroyWindow(h);
      } catch (const fn::Error &e) { MessageBoxW(h, e.message.c_str(), L"FolderNote", MB_OK | MB_ICONERROR); }
    } else if (LOWORD(w) == 17 && HIWORD(w) == CBN_SELCHANGE) {
      auto selected = SendMessageW(themeChoice, CB_GETCURSEL, 0, 0);
      if (selected >= 0 && selected <= 2) { appearance->reset(ui::ThemeMode(selected)); appearance->apply(h); quick::setManagerTheme(managerPage, appearance->mode); contextmenu::setTheme(contextPage, unsigned(appearance->mode)); }
    } else if (LOWORD(w) == 12 && HIWORD(w) == BN_CLICKED) {
      EnableWindow(choice, SendMessageW(navigationCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    } else if (LOWORD(w) == 14 && HIWORD(w) == BN_CLICKED) {
      EnableWindow(dialogCheck, SendMessageW(quickCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    } else if (LOWORD(w) == 18 && HIWORD(w) == BN_CLICKED && preferNewTabCheck) {
      EnableWindow(preferNewTabCheck, SendMessageW(tabsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
    } else if (LOWORD(w) == desktopGroupsToggle && HIWORD(w) == BN_CLICKED) {
      updateDesktopGroupsManager();
      InvalidateRect(desktopGroupsPreview, nullptr, FALSE);
    } else if (LOWORD(w) == desktopGroupsGlassToggle && HIWORD(w) == BN_CLICKED) {
      const auto previous = boxStyle;
      boxStyle.glass = SendMessageW(desktopGroupsGlassCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
      SendDlgItemMessageW(h, boxGlass, BM_SETCHECK, boxStyle.glass ? BST_CHECKED : BST_UNCHECKED, 0);
      previewBoxStyle(h, previous);
      RECT client{}; GetClientRect(body, &client);
      const int previewBottom = scale(desktopGroupsPreviewTop + desktopGroupsPreviewHeight + 8);
      if (previewBottom - scrollPosition > client.bottom) {
        scrollPosition = previewBottom - client.bottom;
        layoutBody();
      }
    } else if (LOWORD(w) == desktopGroupsManage && HIWORD(w) == BN_CLICKED) {
      if (IsWindowEnabled(desktopGroupsManager) && !desktop_groups::showManager())
        MessageBoxW(h, lang::text(L"无法打开桌面盒子管理。"), L"FolderNote", MB_OK | MB_ICONERROR);
    } else if ((LOWORD(w) == boxGlass || LOWORD(w) == boxShadow) && HIWORD(w) == BN_CLICKED) {
      const auto previous = boxStyle;
      boxStyle.glass = SendDlgItemMessageW(h, boxGlass, BM_GETCHECK, 0, 0) == BST_CHECKED;
      boxStyle.shadow = SendDlgItemMessageW(h, boxShadow, BM_GETCHECK, 0, 0) == BST_CHECKED;
      SendMessageW(desktopGroupsGlassCheck, BM_SETCHECK, boxStyle.glass ? BST_CHECKED : BST_UNCHECKED, 0);
      previewBoxStyle(h, previous);
    } else if (LOWORD(w) == boxStyleShortcut && HIWORD(w) == BN_CLICKED) {
      showPage(SettingsPage::BoxAppearance);
      SetFocus(GetDlgItem(h, pageButtons[size_t(SettingsPage::BoxAppearance)]));
    } else if (LOWORD(w) == 16 && HIWORD(w) == BN_CLICKED) {
      showPage(SettingsPage::QuickFolders); SetFocus(GetDlgItem(h, 41));
    } else if (auto button = std::find(pageButtons.begin(), pageButtons.end(), LOWORD(w));
               button != pageButtons.end() && HIWORD(w) == BN_CLICKED) {
      showPage(SettingsPage(button - pageButtons.begin()));
    } else if (LOWORD(w) == 13 && HIWORD(w) == BN_CLICKED) {
      try { copyDiagnostics(h); } catch (const fn::Error &e) { MessageBoxW(h, e.message.c_str(), L"FolderNote", MB_OK | MB_ICONERROR); }
    } else if ((HIWORD(w) == BN_CLICKED && std::any_of(std::begin(findToggles), std::end(findToggles),
                   [&](const auto &toggle) { return toggle.id == LOWORD(w); })) ||
               (HIWORD(w) == CBN_SELCHANGE && (LOWORD(w) == findFolderAction || LOWORD(w) == findFileAction ||
                                             LOWORD(w) == findPosition || LOWORD(w) == findRows))) {
      try {
        saveFindSettings(h);
      } catch (const fn::Error &error) {
        refreshFindSettings(h);
        MessageBoxW(h, error.message.c_str(), L"FolderNote", MB_OK | MB_ICONERROR);
      }
    } else if (LOWORD(w) == automaticUpdateCheck && HIWORD(w) == BN_CLICKED && *app::updateRepository &&
               (!updateWork.valid() || workingAutomatically())) {
      const DWORD enabled = SendDlgItemMessageW(h, automaticUpdateCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
      if (RegSetKeyValueW(HKEY_CURRENT_USER, configKey, automaticUpdateValue, REG_DWORD, &enabled, sizeof(enabled)) != ERROR_SUCCESS) {
        SendDlgItemMessageW(h, automaticUpdateCheck, BM_SETCHECK, automaticUpdatesEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        MessageBoxW(h, lang::text(L"无法保存自动更新设置。"), lang::text(L"FolderNote 更新"), MB_OK | MB_ICONERROR);
      } else if (enabled && IsWindowVisible(h)) {
        checkAutomatically(h);
      }
    } else if (LOWORD(w) == updateButton && HIWORD(w) == BN_CLICKED && !updateWork.valid()) {
      if (*app::updateRepository) startUpdateWork(h, UpdateOperation::Check);
      else MessageBoxW(h, lang::text(L"此构建尚未配置 GitHub 更新源。请使用 GitHub Releases 发布的安装包。"), lang::text(L"FolderNote 更新"), MB_OK | MB_ICONINFORMATION);
    } else if (LOWORD(w) == IDCANCEL) DestroyWindow(h);
    return 0;
  case WM_TIMER: if (w == updateTimer) { finishUpdateWork(h); return 0; } break;
  case WM_CLOSE: if (installingManually()) return 0; break;
  case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = scale(26); return TRUE;
  case WM_VSCROLL: if (reinterpret_cast<HWND>(l) == settingsScroll.window()) settingsScroll.command(w); return 0;
  case WM_HSCROLL:
    if (l) {
      const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
      if (std::any_of(std::begin(boxSliders), std::end(boxSliders), [id](const auto &slider) { return slider.control == id; })) {
        const auto previous = boxStyle;
        for (const auto &slider : boxSliders)
          boxStyle.*slider.value = static_cast<DWORD>(SendDlgItemMessageW(h, slider.control, TBM_GETPOS, 0, 0));
        previewBoxStyle(h, previous);
      }
    }
    return 0;
  case WM_MOUSEWHEEL: if (activePage == SettingsPage::Features) settingsScroll.wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0;
  case WM_SIZE: if (body) layoutSettings(); return 0;
  case WM_GETMINMAXINFO: {
    auto *limits = reinterpret_cast<MINMAXINFO *>(l); limits->ptMinTrackSize = {scale(900), scale(500)}; return 0;
  }
  case WM_SETTINGCHANGE: case WM_THEMECHANGED:
    appearance->reset(appearance->mode); appearance->apply(h); quick::setManagerTheme(managerPage, appearance->mode);
    SendMessageW(entriesPage, WM_THEMECHANGED, 0, 0); return 0;
  case WM_DPICHANGED: {
    UINT oldDpi = dpi; dpi = HIWORD(w); scrollPosition = MulDiv(scrollPosition, dpi, oldDpi);
    auto oldFont = font, oldHeading = headingFont; font = ui::makeFont(dpi); headingFont = ui::makeFont(dpi, 23, FW_SEMIBOLD);
    EnumChildWindows(h, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, value, TRUE); return TRUE; }, reinterpret_cast<LPARAM>(font));
    SendMessageW(GetDlgItem(h, 30), WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    SendMessageW(GetDlgItem(h, 35), WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    for (auto combo : {themeChoice, choice, GetDlgItem(h, languageChoice), GetDlgItem(h, findFolderAction),
         GetDlgItem(h, findFileAction), GetDlgItem(h, findPosition), GetDlgItem(h, findRows)}) {
      SendMessageW(combo, CB_SETITEMHEIGHT, WPARAM(-1), scale(26));
      for (int i = 0; i < SendMessageW(combo, CB_GETCOUNT, 0, 0); ++i) SendMessageW(combo, CB_SETITEMHEIGHT, i, scale(26));
    }
    DeleteObject(oldFont); DeleteObject(oldHeading);
    auto *rect = reinterpret_cast<RECT *>(l);
    SetWindowPos(h, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER);
    SendMessageW(managerPage, WM_DPICHANGED_AFTERPARENT, 0, 0);
    SendMessageW(entriesPage, WM_DPICHANGED_AFTERPARENT, 0, 0);
    layoutSettings(); return 0;
  }
  case WM_DESTROY:
    if (!reopenForLanguage) restoreBoxStylePreview();
    KillTimer(h, updateTimer);
    if (!workingAutomatically()) updateWork = {};
    updateTip = nullptr;
    PostQuitMessage(0);
    return 0;
  case WM_NCDESTROY: RemovePropW(h, app::settingsWindowTag); DeleteObject(font); DeleteObject(headingFont); body = managerPage = contextPage = entriesPage = settingsWindow = nullptr; placements.clear(); break;
  }
  return DefWindowProcW(h, m, w, l);
}
int settings(HINSTANCE instance, SettingsPage page) {
  HWND existing = nullptr;
  EnumWindows([](HWND window, LPARAM data) -> BOOL {
    if (!GetPropW(window, app::settingsWindowTag)) return TRUE;
    *reinterpret_cast<HWND *>(data) = window; return FALSE;
  }, reinterpret_cast<LPARAM>(&existing));
  if (existing) {
    PostMessageW(existing, selectPageMessage(), static_cast<WPARAM>(page), 0);
    ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(existing); return 0;
  }
  activePage = page;
  WNDCLASSW cls{}; cls.lpfnWndProc = contentProc; cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"FolderNote.Settings.Content"; RegisterClassW(&cls);
  cls.lpfnWndProc = settingsProc; cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_FOLDERNOTE));
  cls.lpszClassName = L"FolderNote.Settings"; RegisterClassW(&cls);
  do {
    reopenForLanguage = false;
    appearance = std::make_unique<ui::Theme>(); dpi = GetDpiForSystem();
    const RECT work = ui::mouseWorkArea();
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, cls.lpszClassName, lang::text(L"FolderNote 系统工具设置"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        work.left, work.top, scale(980), scale(660), nullptr, nullptr, instance, nullptr);
    if (!window) {
      restoreBoxStylePreview();
      appearance.reset();
      languageDraft.reset();
      return 1;
    }
    ui::centerLogical(window, work, 980, 660); ShowWindow(window, SW_SHOW); SetFocus(GetDlgItem(window, pageButtons[size_t(activePage)]));
    if (workingAutomatically()) SetTimer(window, updateTimer, 100, nullptr);
    else checkAutomatically(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == WM_KEYDOWN && (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN)) {
        HWND dropped = nullptr;
        for (auto combo : {themeChoice, choice, GetDlgItem(window, languageChoice),
             GetDlgItem(window, findFolderAction), GetDlgItem(window, findFileAction), GetDlgItem(window, findPosition), GetDlgItem(window, findRows),
             explorer_entries_page::locationControl(entriesPage)})
          if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) dropped = combo;
        if (dropped) { SendMessageW(dropped, CB_SHOWDROPDOWN, FALSE, 0); continue; }
        if (message.wParam == VK_ESCAPE) SendMessageW(window, WM_CLOSE, 0, 0);
        else {
          HWND focused = GetFocus(); wchar_t clsName[32]{}; GetClassNameW(focused, clsName, ARRAYSIZE(clsName));
          if (!_wcsicmp(clsName, L"BUTTON")) SendMessageW(focused, BM_CLICK, 0, 0);
          else if (activePage == SettingsPage::Features || activePage == SettingsPage::BoxAppearance)
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        }
        continue;
      }
      if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    appearance.reset();
  } while (reopenForLanguage);
  if (workingAutomatically()) {
    updateWork.wait();
    updateWork = {};
  }
  return 0;
}
} // namespace navigation
