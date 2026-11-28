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
static constexpr wchar_t configKey[] = L"Software\\ShibaKit";
static constexpr wchar_t listenerClass[] = L"ShibaKit.Navigation.Listener.v1";
struct Notification { DWORD process; UINT message; };
static BOOL CALLBACK notifyTab(HWND window, LPARAM message) {
  const auto &notification = *reinterpret_cast<const Notification *>(message);
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  if (process == notification.process && GetPropW(window, L"ShibaKit.Navigation.Controller.v3"))
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
  notifyExplorer(L"ShibaKit.Navigation.Shutdown.v2");
}
void reload() {
  stopLegacy();
  notifyExplorer(L"ShibaKit.Navigation.SettingsChanged.v2");
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
  // Only alter menus already installed by ShibaKit. A modern-only or portable
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
static HWND dblClickParentRadio, dblClickBackRadio;
static HWND notesCheck;
static HWND navigationCheck;
static HWND quickCheck;
static HWND dialogCheck;
static HWND elevatedDialogCheck;
static HWND tabsCheck;
static HWND preferNewTabCheck;
static HWND desktopGroupsCheck, desktopGroupsManager, quickFindCheck;
static HFONT font, sidebarActiveFont, brandFont, headingFont, cardTitleFont, cardDescFont, cardIconFont, smallFont, symbolFont;
static HBRUSH sidebarBrush = nullptr;
static HWND body, themeChoice, settingsWindow, managerPage, contextPage, entriesPage;
static SettingsPage activePage = SettingsPage::Features;
static int settingsSidebarWidth = 208;
static std::vector<int> sidebarDividers;
static int hoveredCard = -1;
static int hoveredCardAction = -1;
static int hoveredSidebarId = 0;
static bool draggingSettingsSidebar = false;
static HWND updateTip;
struct SidebarItem {
  int id;
  SettingsPage page;
  bool conditional;
  int group;
};
static constexpr std::array<SidebarItem, 10> sidebarItems{{
    {40, SettingsPage::Features, false, 0},
    {48, SettingsPage::Preferences, false, 0},
    {77, SettingsPage::Notes, true, 1},
    {59, SettingsPage::DoubleClick, true, 1},
    {41, SettingsPage::QuickFolders, true, 1},
    {50, SettingsPage::BoxAppearance, true, 1},
    {75, SettingsPage::QuickFind, true, 1},
    {76, SettingsPage::Tabs, true, 1},
    {47, SettingsPage::ContextMenus, false, 2},
    {49, SettingsPage::ExplorerEntries, false, 2},
}};
static const wchar_t *sidebarLabel(SettingsPage page) {
  switch (page) {
  case SettingsPage::Features: return lang::text(L"功能总览");
  case SettingsPage::Notes: return lang::text(L"目录备注");
  case SettingsPage::DoubleClick: return lang::text(L"双击导航");
  case SettingsPage::QuickFolders: return lang::text(L"中键定位");
  case SettingsPage::BoxAppearance: return lang::text(L"桌面盒子");
  case SettingsPage::QuickFind: return lang::text(L"快捷搜索");
  case SettingsPage::Tabs: return lang::text(L"多标签栏");
  case SettingsPage::ContextMenus: return lang::text(L"右键菜单");
  case SettingsPage::ExplorerEntries: return lang::text(L"入口管理");
  case SettingsPage::Preferences: return lang::text(L"通用设置");
  }
  return lang::text(L"功能总览");
}
static const wchar_t *sidebarIcon(SettingsPage page) {
  switch (page) {
  case SettingsPage::Features: return L"\xE80F";
  case SettingsPage::Notes: return L"\xE70B";
  case SettingsPage::DoubleClick: return L"\xE74A";
  case SettingsPage::QuickFolders: return L"\xE718";
  case SettingsPage::BoxAppearance: return L"\xE80A";
  case SettingsPage::QuickFind: return L"\xE721";
  case SettingsPage::Tabs: return L"\xE78B";
  case SettingsPage::ContextMenus: return L"\xEA37";
  case SettingsPage::ExplorerEntries: return L"\xE8F4";
  case SettingsPage::Preferences: return L"\xE713";
  }
  return L"\xE80F";
}
static const wchar_t *pageTitle(SettingsPage page) {
  switch (page) {
  case SettingsPage::Features: return lang::text(L"功能总览");
  case SettingsPage::Notes: return lang::text(L"文件夹备注");
  case SettingsPage::QuickFolders: return lang::text(L"中键快速定位");
  case SettingsPage::ContextMenus: return lang::text(L"右键菜单管理");
  case SettingsPage::ExplorerEntries: return lang::text(L"文件夹入口管理");
  case SettingsPage::Preferences: return lang::text(L"通用设置");
  case SettingsPage::BoxAppearance: return lang::text(L"桌面盒子");
  case SettingsPage::QuickFind: return lang::text(L"快捷搜索");
  case SettingsPage::DoubleClick: return lang::text(L"双击空白处导航");
  case SettingsPage::Tabs: return lang::text(L"资源管理器多标签（Windows 10）");
  }
  return lang::text(L"功能总览");
}
static UINT selectPageMessage() { return RegisterWindowMessageW(L"ShibaKit.Settings.SelectPage.v1"); }
static std::unique_ptr<ui::Theme> appearance;
static constexpr int featuresSubtitle = 37;
static constexpr int languageChoice = 43, languageExport = 44, languageImport = 45;
static constexpr int desktopGroupsToggle = 22, desktopGroupsManage = 23, quickFindToggle = 27;
static constexpr int boxGlass = 51, boxShadow = 52, boxRadius = 53, boxRadiusLabel = 55,
                     boxPreview = 57, boxPreviewLabel = 58;
static constexpr int boxShadowOpacity = 60, boxShadowBlur = 61, boxShadowOffset = 62;
static constexpr int boxShadowOpacityLabel = 63, boxShadowBlurLabel = 64, boxShadowOffsetLabel = 65;
static constexpr int cardEditNotes = 101, cardEditDoubleClick = 102, cardEditQuick = 103,
                     cardEditDesktopGroups = 104, cardEditQuickFind = 105, cardEditTabs = 106;
static constexpr int dblClickParent = 10, dblClickBack = 145;
static constexpr int dblClickLabel = 130, dblClickDesc = 131, quickDesc = 132, boxDesc = 133,
                     tabsDesc = 134, tabsNewTabDesc = 135, diagnosticsLabel = 136, diagnosticsDesc = 137,
                     notesDesc = 138, notesMenuLabel = 139, notesCmdEdit = 140, notesCmdClear = 141,
                     notesCmdRepair = 142, notesTip = 143, notesOpenContextMenu = 144,
                     elevatedDialogToggle = 146;
struct FeatureCard {
  SettingsPage detailPage = SettingsPage::Features;
  HWND toggle = nullptr;
  HWND description = nullptr;
  HWND action = nullptr;
  RECT rect{};
  bool visible = true;
};
static std::array<FeatureCard, 6> cards{};
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

static void updateDesktopGroupsManager() {
  if (!desktopGroupsManager) return;
  const bool boxesChecked = desktopGroupsCheck && SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
  if (!importing) wcscpy_s(path.data(), path.size(), L"ShibaKit-language.xml");
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
static constexpr auto quickFindSavedScopesValue = L"QuickFindSavedScopes";
static bool quickFindEnabled(const quick_find::Settings &s) {
  return s.boxes || s.lists || s.explorer || s.dialogs;
}
static DWORD quickFindScopeMask(const quick_find::Settings &s) {
  return (s.boxes ? 1u : 0u) | (s.lists ? 2u : 0u) | (s.explorer ? 4u : 0u) | (s.dialogs ? 8u : 0u);
}
static void syncFeatureNavigation();
static void refreshFindSettings(HWND window) {
  const auto options = quick_find::settings();
  for (const auto &toggle : findToggles)
    SendDlgItemMessageW(window, toggle.id, BM_SETCHECK, options.*toggle.field ? BST_CHECKED : BST_UNCHECKED, 0);
  SendDlgItemMessageW(window, findFolderAction, CB_SETCURSEL, options.openFolders ? 1 : 0, 0);
  SendDlgItemMessageW(window, findFileAction, CB_SETCURSEL, options.openFiles ? 1 : 0, 0);
  SendDlgItemMessageW(window, findPosition, CB_SETCURSEL, options.atPointer ? 0 : 1, 0);
  SendDlgItemMessageW(window, findRows, CB_SETCURSEL, options.visibleRows - 5, 0);
  if (quickFindCheck)
    SendMessageW(quickFindCheck, BM_SETCHECK, quickFindEnabled(options) ? BST_CHECKED : BST_UNCHECKED, 0);
  syncFeatureNavigation();
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
  if (IsWindowVisible(window) && (options.explorer != previous.explorer || options.dialogs != previous.dialogs))
    quick::syncHelper(quick::executable());
  if (quickFindCheck)
    SendMessageW(quickFindCheck, BM_SETCHECK, quickFindEnabled(options) ? BST_CHECKED : BST_UNCHECKED, 0);
  syncFeatureNavigation();
}
static constexpr UINT_PTR updateTimer = 42;
static constexpr auto automaticUpdateValue = L"AutomaticUpdate";
static constexpr auto lastAutomaticCheckValue = L"LastAutomaticUpdateCheck";
static constexpr auto uninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ShibaKit";

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
  auto result = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\Directory\\shell\\ShibaKit.Edit", 0, KEY_QUERY_VALUE, &key);
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
  for (const int id : {languageChoice, languageImport})
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
      MessageBoxW(owner, lang::text(L"无法启动更新，请重试。"), lang::text(L"ShibaKit 更新"), MB_OK | MB_ICONERROR);
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
  auto header = lang::format(L"发现新版本 ShibaKit {0}（当前版本 {1}）。", {release.version, app::version});
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
  constexpr wchar_t dialogClass[] = L"ShibaKit.UpdateConfirmDialog";
  WNDCLASSW cls{};
  if (!GetClassInfoW(instance, dialogClass, &cls)) {
    cls.lpfnWndProc = updateConfirmDialogProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SHIBAKIT));
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
                                lang::text(L"ShibaKit 更新"),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                x, y, dialogW, dialogH, owner, nullptr, instance, &state);
  if (!dialog) return nullptr;

  RECT client{};
  GetClientRect(dialog, &client);
  const int margin = px(20);
  const int contentW = std::max(px(200), static_cast<int>(client.right) - margin * 2);

  const auto headingText = lang::format(L"发现新版本 ShibaKit {0}（当前版本 {1}）", {release.version, app::version});
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
    return MessageBoxW(owner, prompt.c_str(), lang::text(L"ShibaKit 更新"), MB_YESNO | MB_ICONQUESTION) == IDYES;
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
      updateStatus = lang::format(L"已安装 ShibaKit {0}。重新打开设置即可使用新版。", {updatedVersion});
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
    MessageBoxW(owner, result.error.c_str(), lang::text(L"ShibaKit 更新"), MB_OK | MB_ICONERROR);
  } else if (result.release) {
    offerUpdate(owner, *result.release);
  } else {
    MessageBoxW(owner, lang::text(L"当前已是最新版本。"), lang::text(L"ShibaKit 更新"), MB_OK | MB_ICONINFORMATION);
  }
}
static int scrollPosition = 0;
static ui::ScrollBar settingsScroll;
static UINT dpi;
static int scale(int n) { return MulDiv(n, dpi, 96); }
static int contentHeight() {
  const int visibleCount = static_cast<int>(std::count_if(cards.begin(), cards.end(),
      [](const auto &card) { return card.visible; }));
  const int rows = std::max(1, (visibleCount + 1) / 2);
  return scale(8 + rows * 120 + std::max(0, rows - 1) * 12 + 16);
}

static COLORREF blend(COLORREF a, COLORREF b, int weight) {
  const auto channel = [weight](int from, int to) { return (from * (100 - weight) + to * weight) / 100; };
  return RGB(channel(GetRValue(a), GetRValue(b)), channel(GetGValue(a), GetGValue(b)), channel(GetBValue(a), GetBValue(b)));
}

static COLORREF sidebarBackground(const ui::Palette &p) {
  if (p.highContrast) return p.background;
  return p.dark ? blend(p.background, RGB(0, 0, 0), 22) : blend(p.background, p.surface, 52);
}

static void syncSidebarBrush() {
  if (sidebarBrush) {
    DeleteObject(sidebarBrush);
    sidebarBrush = nullptr;
  }
  if (appearance)
    sidebarBrush = CreateSolidBrush(sidebarBackground(appearance->colors));
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
  const bool glass = !colors.highContrast && boxStyle.glass;
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
  MessageBoxW(owner, lang::text(L"诊断信息已复制，可粘贴给开发者排查。"), L"ShibaKit", MB_OK | MB_ICONINFORMATION);
}
static bool featureEnabledForPage(SettingsPage page) {
  switch (page) {
  case SettingsPage::Notes:
    return notesCheck ? SendMessageW(notesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : notesEnabled();
  case SettingsPage::DoubleClick:
    return navigationCheck ? SendMessageW(navigationCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : configuredAction() != Action::Off;
  case SettingsPage::QuickFolders:
    return quickCheck ? SendMessageW(quickCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : quickEnabled();
  case SettingsPage::BoxAppearance:
    return desktopGroupsCheck ? SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : desktop_groups::enabled();
  case SettingsPage::QuickFind:
    return quickFindCheck ? SendMessageW(quickFindCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : quickFindEnabled(quick_find::settings());
  case SettingsPage::Tabs:
    return tabsSupported() && (tabsCheck ? SendMessageW(tabsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : tabsEnabled());
  default:
    return true;
  }
}

static bool sidebarItemVisible(const SidebarItem &item) {
  if (!item.conditional) return true;
  if (item.page == SettingsPage::Tabs && !tabsSupported()) return false;
  if (item.page == activePage) return true;
  return featureEnabledForPage(item.page);
}

static void layoutSettings();

static void refreshNotesControls(HWND owner) {
  if (!owner) return;
  const bool enabled = featureEnabledForPage(SettingsPage::Notes);
  for (int op = 0; op < 3; ++op) {
    HWND checkbox = GetDlgItem(owner, notesCmdEdit + op);
    if (!checkbox) continue;
    SendMessageW(checkbox, BM_SETCHECK, !noteCommandHidden(op) ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(checkbox, enabled);
  }
}

static void syncFeatureNavigation() {
  for (auto &card : cards) {
    if (!card.visible || !card.action) continue;
    EnableWindow(card.action, featureEnabledForPage(card.detailPage));
  }
  if (settingsWindow && body) {
    layoutSettings();
    InvalidateRect(settingsWindow, nullptr, TRUE);
  }
}

static void loadSettingsToControls(HWND owner) {
  SendMessageW(notesCheck, BM_SETCHECK, notesEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
  refreshNotesControls(owner);
  const auto action = configuredAction();
  const auto preferred = preferredAction();
  SendMessageW(navigationCheck, BM_SETCHECK, action != Action::Off ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(dblClickParentRadio, BM_SETCHECK, preferred == Action::Parent ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(dblClickBackRadio, BM_SETCHECK, preferred == Action::Back ? BST_CHECKED : BST_UNCHECKED, 0);
  EnableWindow(dblClickParentRadio, action != Action::Off);
  EnableWindow(dblClickBackRadio, action != Action::Off);
  const bool quick = quickEnabled();
  const bool dialogs = dialogQuickEnabled();
  SendMessageW(quickCheck, BM_SETCHECK, quick ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(dialogCheck, BM_SETCHECK, dialogs ? BST_CHECKED : BST_UNCHECKED, 0);
  EnableWindow(dialogCheck, quick);
  if (elevatedDialogCheck) {
    SendMessageW(elevatedDialogCheck, BM_SETCHECK, elevatedDialogQuickEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(elevatedDialogCheck, quick && dialogs);
  }
  const bool boxes = desktop_groups::enabled();
  SendMessageW(desktopGroupsCheck, BM_SETCHECK, boxes ? BST_CHECKED : BST_UNCHECKED, 0);
  updateDesktopGroupsManager();
  boxStyle = desktop_groups::configuredAppearance();
  SendDlgItemMessageW(owner, boxGlass, BM_SETCHECK, boxStyle.glass ? BST_CHECKED : BST_UNCHECKED, 0);
  SendDlgItemMessageW(owner, boxShadow, BM_SETCHECK, boxStyle.shadow ? BST_CHECKED : BST_UNCHECKED, 0);
  for (const auto &spec : boxSliders)
    SendDlgItemMessageW(owner, spec.control, TBM_SETPOS, TRUE, boxStyle.*spec.value);
  updateBoxStylePreview(owner);
  if (tabsCheck) {
    const bool tabs = tabsEnabled();
    SendMessageW(tabsCheck, BM_SETCHECK, tabs ? BST_CHECKED : BST_UNCHECKED, 0);
    if (preferNewTabCheck) {
      SendMessageW(preferNewTabCheck, BM_SETCHECK, preferNewTabEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
      EnableWindow(preferNewTabCheck, tabs);
    }
  }
  appearance->reset(ui::configuredTheme());
  SendMessageW(themeChoice, CB_SETCURSEL, DWORD(appearance->mode), 0);
  refreshFindSettings(owner);
}

enum class CommitScope { Notes, Navigation, Quick, DesktopGroups, BoxStyle, Tabs, Theme };

static void commitSettings(HWND owner, CommitScope scope) {
  try {
    const bool backSelected = dblClickBackRadio && SendMessageW(dblClickBackRadio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const auto preferred = backSelected ? Action::Back : Action::Parent;
    const bool navOn = SendMessageW(navigationCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const auto action = navOn ? preferred : Action::Off;
    const bool notes = SendMessageW(notesCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool quick = SendMessageW(quickCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool dialogs = SendMessageW(dialogCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool tabs = tabsCheck ? SendMessageW(tabsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : tabsEnabled();
    const bool preferNewTab = preferNewTabCheck ? SendMessageW(preferNewTabCheck, BM_GETCHECK, 0, 0) == BST_CHECKED : preferNewTabEnabled();
    const bool boxes = SendMessageW(desktopGroupsCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    applySettings(notes, action, preferred, quick, dialogs, appearance->mode, tabs, preferNewTab, true,
        DesktopGroupsSettings{boxes, boxStyle.glass, boxStyle.radius, boxStyle.opacity, boxStyle.shadow,
                              boxStyle.shadowOpacity, boxStyle.shadowBlur, boxStyle.shadowOffset});

    EnableWindow(dblClickParentRadio, navOn);
    EnableWindow(dblClickBackRadio, navOn);
    EnableWindow(dialogCheck, quick);
    if (elevatedDialogCheck) EnableWindow(elevatedDialogCheck, quick && dialogs);
    if (preferNewTabCheck) EnableWindow(preferNewTabCheck, tabs);
    refreshNotesControls(owner);
    updateDesktopGroupsManager();
    syncFeatureNavigation();

    if (IsWindowVisible(owner)) {
      switch (scope) {
      case CommitScope::Notes:
      case CommitScope::Navigation:
        reload();
        break;
      case CommitScope::Quick:
        quick::syncHelper(quick::executable());
        break;
      case CommitScope::DesktopGroups:
        if (!desktop_groups::sync())
          throw fn::Error{lang::text(L"设置已保存，但无法应用桌面盒子设置。请再次点击保存。")};
        break;
      case CommitScope::BoxStyle:
        desktop_groups::restoreAppearance();
        boxAppearancePreviewing = false;
        break;
      case CommitScope::Tabs: {
        reload();
        const auto tabsStatus = syncTabs();
        if (tabsSupported() && tabsEnabled() && tabsStatus.windows && tabsStatus.visible == 0) {
          MessageBoxW(owner,
              lang::text(L"设置已保存，已打开的文件夹窗口尚未显示标签栏。\n请关闭后重新打开文件夹窗口。标签栏由这里的开关控制，无需在资源管理器菜单中勾选。\n仍未显示时，可复制诊断信息查看加载状态。"),
              lang::text(L"ShibaKit 多标签"), MB_OK | MB_ICONINFORMATION);
        }
        break;
      }
      case CommitScope::Theme:
        ui::notifyThemeChanged();
        break;
      }
    }
  } catch (const fn::Error &e) {
    loadSettingsToControls(owner);
    if (IsWindowVisible(owner))
      MessageBoxW(owner, e.message.c_str(), L"ShibaKit", MB_OK | MB_ICONERROR);
  } catch (...) {
    loadSettingsToControls(owner);
    if (IsWindowVisible(owner))
      MessageBoxW(owner, lang::text(L"无法保存设置，已恢复原有状态。"), L"ShibaKit", MB_OK | MB_ICONERROR);
  }
}

static void handleQuickFindMasterToggle(HWND owner, bool wantEnabled) {
  const auto previous = quick_find::settings();
  auto options = previous;
  if (!wantEnabled) {
    const DWORD mask = quickFindScopeMask(previous);
    if (mask != 0)
      RegSetKeyValueW(HKEY_CURRENT_USER, configKey, quickFindSavedScopesValue, REG_DWORD, &mask, sizeof(mask));
    options.boxes = options.lists = options.explorer = options.dialogs = false;
  } else {
    DWORD mask = 0, bytes = sizeof(mask);
    if (RegGetValueW(HKEY_CURRENT_USER, configKey, quickFindSavedScopesValue, RRF_RT_REG_DWORD,
                     nullptr, &mask, &bytes) != ERROR_SUCCESS || (mask & 0xF) == 0)
      mask = 0x3;
    options.boxes = (mask & 1u) != 0;
    options.lists = (mask & 2u) != 0;
    options.explorer = (mask & 4u) != 0;
    options.dialogs = (mask & 8u) != 0;
  }
  const auto error = quick_find::saveSettings(options);
  if (error != ERROR_SUCCESS) {
    refreshFindSettings(owner);
    if (IsWindowVisible(owner))
      MessageBoxW(owner, lang::text(L"无法保存快捷搜索设置。"), L"ShibaKit", MB_OK | MB_ICONERROR);
    return;
  }
  refreshFindSettings(owner);
  if (IsWindowVisible(owner) && (options.explorer != previous.explorer || options.dialogs != previous.dialogs)) {
    try {
      quick::syncHelper(quick::executable());
    } catch (const fn::Error &e) {
      MessageBoxW(owner, e.message.c_str(), L"ShibaKit", MB_OK | MB_ICONERROR);
    }
  }
}

static void handleFeatureToggle(HWND owner, int controlId) {
  if (controlId == 11) commitSettings(owner, CommitScope::Notes);
  else if (controlId == 12) commitSettings(owner, CommitScope::Navigation);
  else if (controlId == 14) commitSettings(owner, CommitScope::Quick);
  else if (controlId == desktopGroupsToggle) commitSettings(owner, CommitScope::DesktopGroups);
  else if (controlId == quickFindToggle) {
    const bool want = SendMessageW(quickFindCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    handleQuickFindMasterToggle(owner, want);
  } else if (controlId == 18 && tabsCheck) {
    commitSettings(owner, CommitScope::Tabs);
  }
}

static void layoutBody() {
  if (!body) return;
  RECT client{}; GetClientRect(body, &client);
  scrollPosition = std::clamp(scrollPosition, 0, std::max(0, contentHeight() - int(client.bottom)));
  settingsScroll.configure(contentHeight(), client.bottom, scrollPosition, dpi, scale(16));
  if (activePage != SettingsPage::Features) ShowWindow(settingsScroll.window(), SW_HIDE);
  const int padX = scale(20), padTop = scale(8), gapX = scale(14), gapY = scale(12);
  const int cardH = scale(120), btnW = scale(28), btnH = scale(26);
  const int cardW = std::max(scale(220), (int(client.right) - padX * 2 - gapX) / 2);
  int visibleIndex = 0;
  for (auto &card : cards) {
    if (!card.visible) continue;
    const int col = visibleIndex % 2;
    const int row = visibleIndex / 2;
    ++visibleIndex;
    const int x = padX + col * (cardW + gapX);
    const int y = padTop + row * (cardH + gapY) - scrollPosition;
    card.rect = {x, y, x + cardW, y + cardH};
    MoveWindow(card.toggle, x + scale(72), y + scale(16), std::max(0, cardW - scale(88)), scale(26), FALSE);
    MoveWindow(card.description, x + scale(72), y + scale(45), std::max(0, cardW - scale(88)), scale(36), FALSE);
    MoveWindow(card.action, x + cardW - scale(14) - btnW, y + scale(84), btnW, btnH, FALSE);
  }
  RedrawWindow(body, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static int measureControlWidth(HWND control, HFONT textFont, int extra) {
  const int length = GetWindowTextLengthW(control);
  std::wstring label(size_t(length) + 1, L'\0');
  GetWindowTextW(control, label.data(), length + 1);
  int width = 0;
  if (auto dc = GetDC(control)) {
    auto previous = SelectObject(dc, textFont);
    SIZE extent{};
    if (GetTextExtentPoint32W(dc, label.c_str(), length, &extent))
      width = int(extent.cx) + extra;
    SelectObject(dc, previous);
    ReleaseDC(control, dc);
  }
  return width;
}

static void layoutSettings() {
  RECT client{}; GetClientRect(settingsWindow, &client);
  int width = client.right, height = client.bottom;
  const int sidebar = settingsSidebarWidth;
  MoveWindow(GetDlgItem(settingsWindow, 35), scale(12), scale(27), scale(sidebar - 24), scale(34), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, 31), scale(20), height - scale(39), scale(sidebar - 79), scale(22), TRUE);
  MoveWindow(GetDlgItem(settingsWindow, updateButton), scale(sidebar - 52), height - scale(44), scale(32), scale(32), TRUE);

  int visibleCount = 0;
  int dividerCount = 0;
  int countGroup = -1;
  for (const auto &item : sidebarItems) {
    if (!sidebarItemVisible(item)) continue;
    if (countGroup >= 0 && item.group != countGroup) ++dividerCount;
    countGroup = item.group;
    ++visibleCount;
  }
  const int navTop = scale(88);
  const int navBottom = height - scale(50);
  const int dividerGap = scale(12);
  const int available = std::max(scale(120), navBottom - navTop - dividerCount * dividerGap);
  const int step = std::min(scale(40), std::max(scale(26), available / std::max(1, visibleCount)));
  const int btnH = std::max(scale(24), step - scale(4));
  int navY = navTop;
  int lastGroup = -1;
  sidebarDividers.clear();
  for (const auto &item : sidebarItems) {
    HWND ctrl = GetDlgItem(settingsWindow, item.id);
    if (!sidebarItemVisible(item)) {
      ShowWindow(ctrl, SW_HIDE);
      continue;
    }
    if (lastGroup >= 0 && item.group != lastGroup) {
      sidebarDividers.push_back(navY + scale(4));
      navY += dividerGap;
    }
    lastGroup = item.group;
    MoveWindow(ctrl, scale(12), navY, scale(sidebar - 24), btnH, TRUE);
    ShowWindow(ctrl, SW_SHOWNA);
    navY += step;
  }

  MoveWindow(GetDlgItem(settingsWindow, 30), scale(sidebar + 24), scale(23), std::max(0, width - scale(sidebar + 48)), scale(34), TRUE);
  ShowWindow(GetDlgItem(settingsWindow, 30), activePage == SettingsPage::ContextMenus ? SW_HIDE : SW_SHOWNA);
  MoveWindow(GetDlgItem(settingsWindow, featuresSubtitle), scale(sidebar + 24), scale(59), std::max(0, width - scale(sidebar + 48)), scale(20), TRUE);
  ShowWindow(GetDlgItem(settingsWindow, featuresSubtitle), activePage == SettingsPage::Features ? SW_SHOWNA : SW_HIDE);

  const int left = scale(sidebar + 24);
  const int contentWidth = std::max(0, width - scale(sidebar + 48));
  const int preferencesWidth = std::max(0, MulDiv(width, 96, dpi) - sidebar - 48);

  auto placeForPage = [&](SettingsPage page, int id, int x, int y, int w, int h) {
    auto control = GetDlgItem(settingsWindow, id);
    if (!control) return;
    MoveWindow(control, left + x, scale(y), w, h, TRUE);
    ShowWindow(control, activePage == page ? SW_SHOWNA : SW_HIDE);
  };

  // Notes page
  placeForPage(SettingsPage::Notes, notesDesc, 0, 78, contentWidth, scale(38));
  placeForPage(SettingsPage::Notes, notesMenuLabel, 0, 124, contentWidth, scale(20));
  for (int op = 0; op < 3; ++op) {
    HWND checkbox = GetDlgItem(settingsWindow, notesCmdEdit + op);
    const int boxWidth = checkbox ? std::min(contentWidth, measureControlWidth(checkbox, font, scale(34))) : contentWidth;
    placeForPage(SettingsPage::Notes, notesCmdEdit + op, 0, 150 + op * 30, boxWidth, scale(26));
  }
  placeForPage(SettingsPage::Notes, notesTip, 0, 250, contentWidth, scale(40));
  placeForPage(SettingsPage::Notes, notesOpenContextMenu, 0, 298, scale(172), scale(32));

  // DoubleClick page
  placeForPage(SettingsPage::DoubleClick, dblClickLabel, 0, 84, contentWidth, scale(22));
  const int parentRadioWidth = std::min(contentWidth, measureControlWidth(dblClickParentRadio, font, scale(34)));
  const int backRadioWidth = std::min(contentWidth, measureControlWidth(dblClickBackRadio, font, scale(34)));
  placeForPage(SettingsPage::DoubleClick, dblClickParent, 0, 114, parentRadioWidth, scale(26));
  placeForPage(SettingsPage::DoubleClick, dblClickBack, 0, 146, backRadioWidth, scale(26));
  placeForPage(SettingsPage::DoubleClick, dblClickDesc, 0, 184, contentWidth, scale(40));

  // QuickFolders page
  const int dialogWidth = std::min(contentWidth, measureControlWidth(dialogCheck, font, scale(34)));
  placeForPage(SettingsPage::QuickFolders, 15, 0, 78, dialogWidth, scale(26));
  const int elevatedWidth = elevatedDialogCheck
      ? std::min(std::max(0, contentWidth - scale(24)), measureControlWidth(elevatedDialogCheck, font, scale(34)))
      : 0;
  placeForPage(SettingsPage::QuickFolders, elevatedDialogToggle, scale(24), 108, elevatedWidth, scale(26));
  placeForPage(SettingsPage::QuickFolders, quickDesc, 0, 138, contentWidth, scale(34));

  // Tabs page
  if (preferNewTabCheck) {
    placeForPage(SettingsPage::Tabs, tabsDesc, 0, 84, contentWidth, scale(40));
    const int preferWidth = std::min(contentWidth, measureControlWidth(preferNewTabCheck, font, scale(34)));
    placeForPage(SettingsPage::Tabs, 21, 0, 134, preferWidth, scale(26));
    placeForPage(SettingsPage::Tabs, tabsNewTabDesc, scale(28), 164, std::max(0, contentWidth - scale(28)), scale(42));
  }

  // Preferences (General) page
  auto preference = [&](int id, int x, int y, int w, int h) {
    placeForPage(SettingsPage::Preferences, id, scale(x), y, scale(w), scale(h));
  };
  preference(46, 0, 78, 210, 20);
  preference(languageChoice, 0, 102, 176, 130);
  preference(languageExport, 186, 102, 172, 32);
  preference(languageImport, 368, 102, 172, 32);
  preference(32, 0, 152, 210, 20);
  preference(17, 0, 176, 176, 130);
  preference(automaticUpdateCheck, 0, 226, std::min(210, preferencesWidth), 26);
  preference(automaticUpdateDescription, 0, 256, preferencesWidth, 36);
  constexpr int statusTop = 294, diagTop = 342;
  const int statusHeight = std::clamp(diagTop - statusTop - 6, 18, 42);
  preference(updateStatusLabel, 0, statusTop, preferencesWidth, statusHeight);
  preference(diagnosticsLabel, 0, diagTop, preferencesWidth, 20);
  preference(diagnosticsDesc, 0, diagTop + 22, preferencesWidth, 22);
  preference(13, 0, diagTop + 50, 152, 32);

  // QuickFind page
  const int findColumn = (preferencesWidth - 24) / 2;
  auto findControl = [&](int id, int x, int y, int w, int h) {
    placeForPage(SettingsPage::QuickFind, id, scale(x), y, scale(w), scale(h));
  };
  findControl(findDescription, 0, 78, preferencesWidth, 36);
  findControl(findScope, 0, 120, preferencesWidth, 20);
  findControl(findBoxes, 0, 144, findColumn, 26);
  findControl(findLists, findColumn + 24, 144, findColumn, 26);
  findControl(findExplorer, 0, 174, findColumn, 26);
  findControl(findDialogs, findColumn + 24, 174, findColumn, 26);
  for (const auto ids : {std::pair{findFolderLabel, findFolderAction}, std::pair{findFileLabel, findFileAction},
                         std::pair{findPositionLabel, findPosition}, std::pair{findRowsLabel, findRows}}) {
    const bool second = ids.first == findFileLabel || ids.first == findRowsLabel;
    const int y = ids.first < findPositionLabel ? 210 : 280;
    findControl(ids.first, second ? findColumn + 24 : 0, y, findColumn, 20);
    findControl(ids.second, second ? findColumn + 24 : 0, y + 24, findColumn, 200);
  }
  findControl(findIcons, 0, 348, preferencesWidth, 26);
  findControl(findHint, 0, 380, preferencesWidth, 32);

  // BoxAppearance page
  const int boxGap = scale(24);
  const int boxColumn = std::max(0, (contentWidth - boxGap) / 2);
  const int manageWidth = scale(168);
  placeForPage(SettingsPage::BoxAppearance, boxDesc, 0, 78, std::max(0, contentWidth - manageWidth - scale(16)), scale(40));
  placeForPage(SettingsPage::BoxAppearance, desktopGroupsManage, std::max(0, contentWidth - manageWidth), 81, manageWidth, scale(32));
  placeForPage(SettingsPage::BoxAppearance, boxGlass, 0, 128, std::min(boxColumn, scale(224)), scale(26));
  placeForPage(SettingsPage::BoxAppearance, boxShadow, boxColumn + boxGap, 128, std::min(boxColumn, scale(224)), scale(26));
  for (size_t index = 0; index < std::size(boxSliders); ++index) {
    const auto &slider = boxSliders[index];
    const int x = index == 0 ? 0 : boxColumn + boxGap;
    const int y = index == 0 ? 162 : 162 + 45 * static_cast<int>(index - 1);
    placeForPage(SettingsPage::BoxAppearance, slider.label, x, y, boxColumn, scale(19));
    placeForPage(SettingsPage::BoxAppearance, slider.control, x, y + 19, boxColumn, scale(23));
  }
  placeForPage(SettingsPage::BoxAppearance, boxPreviewLabel, 0, 303, contentWidth, scale(20));
  const int previewHeight = std::clamp(height - scale(341), scale(92), scale(165));
  placeForPage(SettingsPage::BoxAppearance, boxPreview, 0, 325, contentWidth, previewHeight);

  const int bodyHeight = std::max(scale(40), height - scale(96));
  const bool overflow = bodyHeight < contentHeight();
  const int contentLeft = scale(sidebar + 5);
  MoveWindow(body, contentLeft, scale(86), width - contentLeft - (overflow ? scale(20) : 0), bodyHeight, TRUE);
  MoveWindow(managerPage, contentLeft, scale(176), width - contentLeft, std::max(scale(120), height - scale(182)), TRUE);
  MoveWindow(contextPage, contentLeft, 0, width - contentLeft, height, TRUE);
  MoveWindow(entriesPage, contentLeft, scale(76), width - contentLeft, std::max(0, height - scale(76)), TRUE);
  settingsScroll.move(width - scale(18), scale(86), scale(12), bodyHeight);
  layoutBody();
  ShowWindow(settingsScroll.window(), activePage == SettingsPage::Features && overflow ? SW_SHOWNA : SW_HIDE);
}

static int cardIndexAtPoint(POINT pt) {
  for (size_t i = 0; i < cards.size(); ++i) {
    if (cards[i].visible && PtInRect(&cards[i].rect, pt)) return int(i);
  }
  return -1;
}

static void setHoveredCard(int index) {
  if (index == hoveredCard) return;
  const int previous = hoveredCard;
  hoveredCard = index;
  if (!body) return;
  auto invalidate = [](int idx) {
    if (idx < 0 || size_t(idx) >= cards.size() || !cards[size_t(idx)].visible) return;
    RECT rect = cards[size_t(idx)].rect;
    InflateRect(&rect, 2, 2);
    InvalidateRect(body, &rect, FALSE);
    if (cards[size_t(idx)].action) InvalidateRect(cards[size_t(idx)].action, nullptr, FALSE);
  };
  invalidate(previous);
  invalidate(hoveredCard);
}

static LRESULT CALLBACK cardChildProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  const int cardIndex = int(data);
  const bool isAction = cardIndex >= 0 && size_t(cardIndex) < cards.size() && h == cards[size_t(cardIndex)].action;
  if (isAction && message == WM_ERASEBKGND) return 1;
  if (isAction && message == WM_SETCURSOR && IsWindowEnabled(h)) {
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    return TRUE;
  }
  if (message == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
    TrackMouseEvent(&track);
    setHoveredCard(cardIndex);
    if (isAction && hoveredCardAction != cardIndex) {
      hoveredCardAction = cardIndex;
      InvalidateRect(h, nullptr, FALSE);
    }
  } else if (message == WM_MOUSELEAVE) {
    if (isAction && hoveredCardAction == cardIndex) {
      hoveredCardAction = -1;
      InvalidateRect(h, nullptr, FALSE);
    }
    if (body) {
      POINT pt{};
      GetCursorPos(&pt);
      ScreenToClient(body, &pt);
      RECT client{};
      GetClientRect(body, &client);
      setHoveredCard(PtInRect(&client, pt) ? cardIndexAtPoint(pt) : -1);
    } else {
      setHoveredCard(-1);
    }
  } else if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(h, cardChildProc, id);
  }
  return DefSubclassProc(h, message, w, l);
}

static void showPage(SettingsPage page) {
  if (page == SettingsPage::Tabs && !tabsSupported()) page = SettingsPage::Features;
  if (page != SettingsPage::Features) {
    hoveredCardAction = -1;
    setHoveredCard(-1);
  }
  activePage = page;
  const std::array pages{body, managerPage, contextPage, entriesPage};
  HWND target = nullptr;
  if (page == SettingsPage::Features) target = body;
  else if (page == SettingsPage::QuickFolders) target = managerPage;
  else if (page == SettingsPage::ContextMenus) target = contextPage;
  else if (page == SettingsPage::ExplorerEntries) target = entriesPage;
  for (auto panel : pages)
    if (panel && panel != target) ShowWindow(panel, SW_HIDE);
  SetWindowTextW(GetDlgItem(settingsWindow, 30), pageTitle(page));
  layoutSettings();
  for (const auto &item : sidebarItems)
    if (sidebarItemVisible(item))
      RedrawWindow(GetDlgItem(settingsWindow, item.id), nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  InvalidateRect(settingsWindow, nullptr, TRUE);
  if (target) ShowWindow(target, SW_SHOWNA);
  if (page == SettingsPage::QuickFolders) quick::refreshManagerPage(managerPage);
  if (page == SettingsPage::ExplorerEntries) explorer_entries_page::refresh(entriesPage);
}

static LRESULT CALLBACK sidebarProc(HWND h, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_SETCURSOR) {
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    return TRUE;
  }
  if (message == WM_MOUSEMOVE) {
    const int ctrl = GetDlgCtrlID(h);
    if (hoveredSidebarId != ctrl) {
      hoveredSidebarId = ctrl;
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
      TrackMouseEvent(&track);
      InvalidateRect(h, nullptr, FALSE);
    }
  } else if (message == WM_MOUSELEAVE) {
    if (hoveredSidebarId == GetDlgCtrlID(h)) {
      hoveredSidebarId = 0;
      InvalidateRect(h, nullptr, FALSE);
    }
  } else if (message == WM_KEYDOWN && GetDlgCtrlID(h) != updateButton &&
             (w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END)) {
    std::vector<const SidebarItem *> visible;
    for (const auto &item : sidebarItems)
      if (sidebarItemVisible(item)) visible.push_back(&item);
    if (visible.empty()) return 0;
    const int ctrl = GetDlgCtrlID(h);
    const auto it = std::find_if(visible.begin(), visible.end(),
        [ctrl](const auto *entry) { return entry->id == ctrl; });
    const int count = int(visible.size());
    const int current = it == visible.end() ? 0 : int(it - visible.begin());
    const int next = w == VK_HOME ? 0 : w == VK_END ? count - 1 : (current + (w == VK_UP ? count - 1 : 1)) % count;
    showPage(visible[next]->page);
    SetFocus(GetDlgItem(settingsWindow, visible[next]->id));
    return 0;
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(h, sidebarProc, id);
  return DefSubclassProc(h, message, w, l);
}

static LRESULT CALLBACK contentProc(HWND h, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_ERASEBKGND) return 1;
  LRESULT result = 0;
  if (appearance && appearance->handle(h, message, w, l, result)) return result;
  switch (message) {
  case WM_SETCURSOR:
    if (hoveredCard != -1 && reinterpret_cast<HWND>(w) != body && GetParent(reinterpret_cast<HWND>(w)) != body)
      setHoveredCard(-1);
    if (LOWORD(l) == HTCLIENT) {
      POINT pt{};
      GetCursorPos(&pt);
      ScreenToClient(h, &pt);
      const int idx = cardIndexAtPoint(pt);
      setHoveredCard(idx);
      if (reinterpret_cast<HWND>(w) == h && idx >= 0 && featureEnabledForPage(cards[size_t(idx)].detailPage)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
    }
    break;
  case WM_MOUSEMOVE: {
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
    TrackMouseEvent(&track);
    setHoveredCard(cardIndexAtPoint({GET_X_LPARAM(l), GET_Y_LPARAM(l)}));
    return 0;
  }
  case WM_MOUSELEAVE: {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(h, &pt);
    RECT client{};
    GetClientRect(h, &client);
    setHoveredCard(PtInRect(&client, pt) ? cardIndexAtPoint(pt) : -1);
    return 0;
  }
  case WM_LBUTTONUP: {
    const int idx = cardIndexAtPoint({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
    if (idx >= 0) {
      const auto targetPage = cards[size_t(idx)].detailPage;
      if (featureEnabledForPage(targetPage)) {
        showPage(targetPage);
        auto it = std::find_if(sidebarItems.begin(), sidebarItems.end(),
            [targetPage](const auto &entry) { return entry.page == targetPage; });
        if (it != sidebarItems.end()) SetFocus(GetDlgItem(settingsWindow, it->id));
      }
    }
    return 0;
  }
  case WM_DRAWITEM: {
    auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
    if (appearance && item->CtlType == ODT_BUTTON && item->CtlID >= cardEditNotes && item->CtlID <= cardEditTabs) {
      const auto &colors = appearance->colors;
      const int cardIdx = int(item->CtlID) - cardEditNotes;
      const bool enabled = (item->itemState & ODS_DISABLED) == 0;
      const bool cardHov = hoveredCard == cardIdx;
      const bool btnHov = hoveredCardAction == cardIdx;
      const bool pressed = (item->itemState & ODS_SELECTED) != 0;
      const bool focused = (item->itemState & ODS_FOCUS) != 0;
      const bool keyboardFocus = focused && !pressed && GetKeyState(VK_LBUTTON) >= 0 && !(SendMessageW(item->hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
      const bool showIcon = enabled && (cardHov || btnHov || pressed || keyboardFocus);
      RECT rc = item->rcItem;
      const int bufW = rc.right - rc.left, bufH = rc.bottom - rc.top;
      HDC bufferDc = bufW > 0 && bufH > 0 ? CreateCompatibleDC(item->hDC) : nullptr;
      HBITMAP bufferBmp = bufferDc ? CreateCompatibleBitmap(item->hDC, bufW, bufH) : nullptr;
      HGDIOBJ oldBmp = bufferBmp ? SelectObject(bufferDc, bufferBmp) : nullptr;
      if (!oldBmp && bufferDc) {
        DeleteDC(bufferDc); bufferDc = nullptr;
        if (bufferBmp) { DeleteObject(bufferBmp); bufferBmp = nullptr; }
      }
      HDC dc = bufferDc ? bufferDc : item->hDC;
      RECT drawRc = bufferDc ? RECT{0, 0, bufW, bufH} : rc;
      FillRect(dc, &drawRc, appearance->card);

      if (showIcon) {
        COLORREF fill = colors.surface, border = colors.surface;
        COLORREF iconColor = (btnHov || pressed) ? colors.accent
            : (colors.highContrast ? colors.text : blend(colors.text, colors.muted, 20));
        if (colors.highContrast) {
          fill = (btnHov || pressed) ? colors.accent : colors.surface;
          border = (keyboardFocus || btnHov) ? colors.accent : colors.surface;
          if (btnHov || pressed) iconColor = colors.onAccent;
        } else {
          if (pressed) {
            fill = blend(colors.surface, colors.text, colors.dark ? 12 : 9);
            border = fill;
          } else if (btnHov) {
            fill = blend(colors.surface, colors.text, colors.dark ? 8 : 5);
            border = fill;
          }
          if (keyboardFocus) border = colors.accent;
        }
        if (fill != colors.surface || border != colors.surface)
          ui::rounded(dc, drawRc, fill, border, scale(6));
        auto oldSym = SelectObject(dc, symbolFont);
        ui::text(dc, L"\xE70F", drawRc, iconColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldSym);
      }

      if (bufferDc) {
        BitBlt(item->hDC, rc.left, rc.top, bufW, bufH, bufferDc, 0, 0, SRCCOPY);
        SelectObject(bufferDc, oldBmp);
        DeleteObject(bufferBmp);
        DeleteDC(bufferDc);
      }
      return TRUE;
    }
    break;
  }
  case WM_PAINT: {
    PAINTSTRUCT paint{};
    HDC paintDc = BeginPaint(h, &paint);
    RECT client{};
    GetClientRect(h, &client);
    const int bufW = client.right - client.left, bufH = client.bottom - client.top;
    HDC bufferDc = bufW > 0 && bufH > 0 ? CreateCompatibleDC(paintDc) : nullptr;
    HBITMAP bufferBmp = bufferDc ? CreateCompatibleBitmap(paintDc, bufW, bufH) : nullptr;
    HGDIOBJ oldBmp = bufferBmp ? SelectObject(bufferDc, bufferBmp) : nullptr;
    if (!oldBmp && bufferDc) {
      DeleteDC(bufferDc); bufferDc = nullptr;
      if (bufferBmp) { DeleteObject(bufferBmp); bufferBmp = nullptr; }
    }
    HDC dc = bufferDc ? bufferDc : paintDc;
    FillRect(dc, &client, appearance->background);
    const auto &colors = appearance->colors;
    const COLORREF restBorder = colors.highContrast
        ? colors.border : blend(colors.surface, colors.border, colors.dark ? 52 : 62);
    const COLORREF hoverBorder = colors.highContrast
        ? colors.text : blend(colors.surface, colors.muted, colors.dark ? 40 : 42);
    for (size_t i = 0; i < cards.size(); ++i) {
      const auto &card = cards[i];
      if (!card.visible) continue;
      const bool hovered = int(i) == hoveredCard;
      const bool enabled = featureEnabledForPage(card.detailPage);
      if (!colors.highContrast && !colors.dark) {
        RECT shadow = card.rect;
        OffsetRect(&shadow, 0, scale(1));
        const COLORREF shadowColor = blend(colors.background, colors.muted, hovered ? 22 : 12);
        ui::rounded(dc, shadow, shadowColor, shadowColor, scale(10));
      }
      ui::rounded(dc, card.rect, colors.surface, hovered ? hoverBorder : restBorder, scale(10));
      if (!colors.highContrast && colors.dark) {
        RECT spec{card.rect.left + scale(10), card.rect.top + 1, card.rect.right - scale(10), card.rect.top + 2};
        SetDCBrushColor(dc, blend(colors.surface, RGB(255, 255, 255), hovered ? 11 : 5));
        FillRect(dc, &spec, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      }
      RECT badge{card.rect.left + scale(16), card.rect.top + scale(18),
                 card.rect.left + scale(58), card.rect.top + scale(60)};
      const COLORREF badgeFill = colors.highContrast ? colors.surface
          : blend(colors.surface, colors.background, colors.dark ? 45 : 55);
      const COLORREF badgeBorder = hovered ? hoverBorder : restBorder;
      const COLORREF glyphColor = enabled ? (hovered ? colors.text : blend(colors.text, colors.muted, 25)) : colors.muted;
      ui::rounded(dc, badge, badgeFill, badgeBorder, scale(10));
      auto oldSym = SelectObject(dc, cardIconFont ? cardIconFont : symbolFont);
      ui::text(dc, sidebarIcon(card.detailPage), badge, glyphColor,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(dc, oldSym);
    }
    if (bufferDc) {
      BitBlt(paintDc, 0, 0, bufW, bufH, bufferDc, 0, 0, SRCCOPY);
      SelectObject(bufferDc, oldBmp);
      DeleteObject(bufferBmp);
      DeleteDC(bufferDc);
    }
    EndPaint(h, &paint);
    return 0;
  }
  case WM_COMMAND: {
    HWND control = reinterpret_cast<HWND>(l);
    if (control && GetFocus() == control) {
      for (const auto &card : cards) {
        if (!card.visible || (control != card.toggle && control != card.action)) continue;
        RECT client{}; GetClientRect(h, &client);
        const int cardTop = card.rect.top + scrollPosition;
        const int cardBottom = card.rect.bottom + scrollPosition;
        if (card.rect.top < 0) scrollPosition = std::max(0, cardTop - scale(8));
        else if (card.rect.bottom > client.bottom) scrollPosition = cardBottom + scale(8) - client.bottom;
        layoutBody();
        break;
      }
    }
    return SendMessageW(GetParent(h), message, w, l);
  }
  case WM_VSCROLL:
    settingsScroll.command(w);
    return 0;
  case WM_MOUSEWHEEL:
    if (activePage == SettingsPage::Features) settingsScroll.wheel(GET_WHEEL_DELTA_WPARAM(w));
    return 0;
  }
  return DefWindowProcW(h, message, w, l);
}
static LRESULT CALLBACK settingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == selectPageMessage()) {
    const auto requested = w <= static_cast<WPARAM>(SettingsPage::Notes) ? SettingsPage(w) : SettingsPage::Features;
    showPage(requested);
    return 0;
  }
  if (m == ui::themeChangedMessage() && appearance) {
    appearance->reset(ui::configuredTheme());
    syncSidebarBrush();
    contextmenu::setTheme(contextPage, unsigned(appearance->mode));
    SendMessageW(themeChoice, CB_SETCURSEL, DWORD(appearance->mode), 0);
    appearance->apply(h);
    quick::setManagerTheme(managerPage, appearance->mode);
    SendMessageW(entriesPage, WM_THEMECHANGED, 0, 0);
    return 0;
  }
  if (m == WM_CTLCOLORSTATIC && appearance && sidebarBrush) {
    HWND ctrl = reinterpret_cast<HWND>(l);
    const int id = GetDlgCtrlID(ctrl);
    if (id == 35 || id == 31) {
      HDC dc = reinterpret_cast<HDC>(w);
      const auto &p = appearance->colors;
      SetBkColor(dc, sidebarBackground(p));
      SetTextColor(dc, id == 35 ? p.text : p.muted);
      return reinterpret_cast<LRESULT>(sidebarBrush);
    }
  }
  if (m == WM_DRAWITEM && appearance) {
    auto *item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
    if (item->CtlID == boxPreview) {
      paintDesktopGroupsPreview(*item);
      return TRUE;
    }
    if (item->CtlType == ODT_BUTTON && item->CtlID == updateButton) {
      const auto &p = appearance->colors;
      const COLORREF sidebarBg = sidebarBackground(p);
      const bool selected = (item->itemState & ODS_SELECTED) != 0;
      const bool disabled = (item->itemState & ODS_DISABLED) != 0;
      const bool hovered = hoveredSidebarId == updateButton || (item->itemState & ODS_HOTLIGHT) != 0;
      const bool focused = (item->itemState & ODS_FOCUS) != 0;
      const bool keyboardFocus = focused && !selected && GetKeyState(VK_LBUTTON) >= 0 && !(SendMessageW(item->hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
      if (sidebarBrush) FillRect(item->hDC, &item->rcItem, sidebarBrush);
      else FillRect(item->hDC, &item->rcItem, appearance->background);
      RECT rect = item->rcItem;
      InflateRect(&rect, -scale(2), -scale(2));
      const COLORREF fill = p.highContrast
          ? (selected ? p.pressed : hovered ? p.hover : sidebarBg)
          : (selected ? blend(sidebarBg, p.text, p.dark ? 14 : 10) : hovered ? blend(sidebarBg, p.text, p.dark ? 8 : 6) : sidebarBg);
      const COLORREF border = p.highContrast
          ? (keyboardFocus ? p.accent : p.border)
          : (keyboardFocus ? p.accent : fill);
      if (fill != sidebarBg || border != sidebarBg || p.highContrast)
        ui::rounded(item->hDC, rect, fill, border, scale(6));
      auto old = SelectObject(item->hDC, symbolFont);
      const auto color = disabled ? p.muted : availableUpdate ? p.accent : (hovered ? p.text : p.muted);
      ui::text(item->hDC, availableUpdate ? L"\xE896" : L"\xE72C", item->rcItem, color,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(item->hDC, old);
      return TRUE;
    }
    auto button = std::find_if(sidebarItems.begin(), sidebarItems.end(),
        [id = int(item->CtlID)](const auto &entry) { return entry.id == id; });
    if (item->CtlType == ODT_BUTTON && button != sidebarItems.end()) {
      const auto &p = appearance->colors;
      const COLORREF sidebarBg = sidebarBackground(p);
      const auto target = button->page;
      const bool selected = target == activePage;
      const bool pressed = (item->itemState & ODS_SELECTED) != 0;
      const bool hovered = !selected && (hoveredSidebarId == button->id || (item->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0);
      const bool focused = (item->itemState & ODS_FOCUS) != 0;
      const bool keyboardFocus = focused && !pressed && GetKeyState(VK_LBUTTON) >= 0 && !(SendMessageW(item->hwndItem, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
      RECT rc = item->rcItem;
      const int bufW = rc.right - rc.left, bufH = rc.bottom - rc.top;
      HDC bufferDc = bufW > 0 && bufH > 0 ? CreateCompatibleDC(item->hDC) : nullptr;
      HBITMAP bufferBmp = bufferDc ? CreateCompatibleBitmap(item->hDC, bufW, bufH) : nullptr;
      HGDIOBJ oldBmp = bufferBmp ? SelectObject(bufferDc, bufferBmp) : nullptr;
      if (!oldBmp && bufferDc) {
        DeleteDC(bufferDc); bufferDc = nullptr;
        if (bufferBmp) { DeleteObject(bufferBmp); bufferBmp = nullptr; }
      }
      HDC dc = bufferDc ? bufferDc : item->hDC;
      RECT rect = bufferDc ? RECT{0, 0, bufW, bufH} : rc;
      if (sidebarBrush) FillRect(dc, &rect, sidebarBrush);
      else FillRect(dc, &rect, appearance->background);
      InflateRect(&rect, -scale(2), -scale(1));
      COLORREF iconColor = p.muted, textColor = p.text;
      if (p.highContrast) {
        const auto fill = selected ? p.accent : hovered ? p.hover : sidebarBg;
        ui::rounded(dc, rect, fill, keyboardFocus && !selected ? p.accent : fill, scale(6));
        iconColor = textColor = selected ? p.onAccent : p.text;
      } else if (selected) {
        ui::rounded(dc, rect, p.accent, p.accent, scale(7));
        iconColor = p.onAccent;
        textColor = p.onAccent;
      } else {
        const COLORREF fill = hovered ? blend(sidebarBg, p.text, p.dark ? 6 : 4) : sidebarBg;
        const COLORREF border = keyboardFocus ? p.accent : fill;
        if (fill != sidebarBg || keyboardFocus)
          ui::rounded(dc, rect, fill, border, scale(7));
        iconColor = hovered ? blend(p.text, p.muted, p.dark ? 15 : 12)
                            : blend(sidebarBg, p.muted, p.dark ? 78 : 76);
        textColor = hovered ? blend(p.text, p.muted, p.dark ? 15 : 12)
                            : blend(p.text, p.muted, p.dark ? 68 : 65);
      }
      const wchar_t *label = sidebarLabel(target);
      HFONT labelFont = (selected && !p.highContrast && sidebarActiveFont) ? sidebarActiveFont : font;
      auto oldFont = SelectObject(dc, labelFont);
      SIZE extent{};
      GetTextExtentPoint32W(dc, label, static_cast<int>(wcslen(label)), &extent);
      const int iconW = scale(18), gap = scale(8);
      const int maxW = std::max(0, int(rect.right - rect.left) - scale(16));
      const int contentW = std::min(maxW, iconW + gap + int(extent.cx));
      const int startX = rect.left + (int(rect.right - rect.left) - contentW) / 2;
      RECT iconRect{startX, rect.top, startX + iconW, rect.bottom};
      SelectObject(dc, symbolFont);
      ui::text(dc, sidebarIcon(target), iconRect, iconColor,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      RECT textRect{startX + iconW + gap, rect.top, std::min(int(rect.right) - scale(8), startX + contentW + scale(2)), rect.bottom};
      SelectObject(dc, labelFont);
      ui::text(dc, label, textRect, textColor,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      SelectObject(dc, oldFont);
      if (bufferDc) {
        BitBlt(item->hDC, rc.left, rc.top, bufW, bufH, bufferDc, 0, 0, SRCCOPY);
        SelectObject(bufferDc, oldBmp);
        DeleteObject(bufferBmp);
        DeleteDC(bufferDc);
      }
      return TRUE;
    }
  }
  LRESULT result = 0;
  if (appearance && appearance->handle(h, m, w, l, result)) return result;
  switch (m) {
  case WM_ENTERSIZEMOVE:
    hoveredCardAction = -1;
    setHoveredCard(-1);
    hoveredSidebarId = 0;
    draggingSettingsSidebar = false;
    break;
  case WM_SETCURSOR:
    if (hoveredCard != -1 && reinterpret_cast<HWND>(w) != body && GetParent(reinterpret_cast<HWND>(w)) != body)
      setHoveredCard(-1);
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
    const auto &colors = appearance->colors;
    auto sidebar = client;
    sidebar.right = scale(settingsSidebarWidth);
    RECT rightPane{sidebar.right, client.top, client.right, client.bottom};
    FillRect(dc, &rightPane, appearance->background);
    if (sidebarBrush) FillRect(dc, &sidebar, sidebarBrush);
    else FillRect(dc, &sidebar, appearance->background);
    RECT separator{sidebar.right - 1, client.top, sidebar.right, client.bottom};
    SetDCBrushColor(dc, colors.highContrast ? colors.border : blend(colors.background, colors.border, colors.dark ? 65 : 75));
    FillRect(dc, &separator, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (!sidebarDividers.empty()) {
      const COLORREF sidebarBg = sidebarBackground(colors);
      SetDCBrushColor(dc, colors.highContrast ? colors.border : blend(sidebarBg, colors.border, colors.dark ? 55 : 65));
      for (const int divY : sidebarDividers) {
        RECT sidebarDivider{scale(16), divY, sidebar.right - scale(16), divY + 1};
        FillRect(dc, &sidebarDivider, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      }
    }
    EndPaint(h, &paint);
    return 0;
  }
  case WM_CREATE: {
    SetPropW(h, app::settingsWindowTag, reinterpret_cast<HANDLE>(1));
    settingsWindow = h; dpi = GetDpiForWindow(h); scrollPosition = 0; cards = {}; sidebarDividers.clear();
    hoveredCard = -1; hoveredCardAction = -1; hoveredSidebarId = 0;
    font = ui::makeFont(dpi, 14, FW_NORMAL);
    sidebarActiveFont = ui::makeFont(dpi, 14, FW_SEMIBOLD);
    brandFont = ui::makeFont(dpi, 18, FW_SEMIBOLD);
    headingFont = ui::makeFont(dpi, 18, FW_LIGHT);
    cardTitleFont = ui::makeFont(dpi, 14, FW_SEMIBOLD);
    cardDescFont = ui::makeFont(dpi, 12, FW_NORMAL);
    cardIconFont = ui::makeSymbolFont(dpi, 22);
    smallFont = ui::makeFont(dpi, 13, FW_NORMAL);
    symbolFont = ui::makeSymbolFont(dpi, 14);
    syncSidebarBrush();
    auto add = [&](HWND parent, LPCWSTR cls, LPCWSTR text, DWORD style, int id) {
      HWND control = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
          parent, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); return control;
    };
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
    boxStyle = desktop_groups::configuredAppearance();
    constexpr DWORD appearanceCheck = BS_AUTOCHECKBOX | WS_TABSTOP;
    const auto glassLabel = std::wstring(lang::text(L"毛玻璃效果")) + L" (Acrylic)";
    auto glassBox = add(h, L"BUTTON", glassLabel.c_str(), appearanceCheck, boxGlass);
    auto shadowBox = add(h, L"BUTTON", lang::text(L"投影"), appearanceCheck, boxShadow);
    ui::toggleSwitch(glassBox);
    ui::toggleSwitch(shadowBox);
    for (const auto &spec : boxSliders) {
      add(h, L"STATIC", L"", 0, spec.label);
      const auto slider = add(h, TRACKBAR_CLASSW, lang::text(spec.name), TBS_NOTICKS | WS_TABSTOP, spec.control);
      SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(spec.min, spec.max));
    }
    ui::muted(add(h, L"STATIC", lang::text(L"预览"), 0, boxPreviewLabel));
    add(h, L"STATIC", lang::text(L"盒子样式预览"), SS_OWNERDRAW, boxPreview);
    ui::muted(add(h, L"STATIC", lang::text(L"由 Windows 管理桌面图标。拖入盒子的桌面文件和文件夹会移到隐藏的 .desktop 目录。"), SS_NOPREFIX, boxDesc));
    desktopGroupsManager = add(h, L"BUTTON", lang::text(L"管理桌面盒子"), WS_TABSTOP | BS_NOTIFY, desktopGroupsManage);

    HWND logo = add(h, L"STATIC", L"ShibaKit", SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, 35);
    SendMessageW(logo, WM_SETFONT, reinterpret_cast<WPARAM>(brandFont), TRUE);
    for (const auto &item : sidebarItems) {
      auto button = add(h, L"BUTTON", sidebarLabel(item.page), BS_OWNERDRAW | WS_TABSTOP, item.id);
      SetWindowSubclass(button, sidebarProc, 1, 0);
    }

    HWND title = add(h, L"STATIC", pageTitle(activePage), SS_NOPREFIX | SS_ENDELLIPSIS, 30);
    SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), TRUE);
    HWND featSub = add(h, L"STATIC", lang::text(L"一键开关立即生效 · 开启功能后左侧显示对应参数菜单"), SS_NOPREFIX | SS_ENDELLIPSIS, featuresSubtitle);
    SendMessageW(featSub, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), TRUE);
    ui::muted(featSub);

    const auto version = lang::format(L"版本 {0} · x64", {app::version});
    HWND verCtrl = add(h, L"STATIC", version.c_str(), SS_LEFT | SS_NOPREFIX, 31);
    SendMessageW(verCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), TRUE);
    ui::muted(verCtrl);
    auto update = add(h, L"BUTTON", lang::text(L"检查更新"), WS_TABSTOP | BS_OWNERDRAW, updateButton);
    SetWindowSubclass(update, sidebarProc, 1, 0);
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

    constexpr DWORD comboStyle = CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP;
    // Notes page controls
    ui::muted(add(h, L"STATIC", lang::text(L"开启后在文件夹右键菜单中显示所选备注命令。关闭功能只隐藏入口，已有备注会保留。"), SS_NOPREFIX, notesDesc));
    ui::muted(add(h, L"STATIC", lang::text(L"右键菜单命令"), SS_NOPREFIX, notesMenuLabel));
    for (int op = 0; op < 3; ++op)
      add(h, L"BUTTON", lang::text(fn::titles[op]), BS_AUTOCHECKBOX | WS_TABSTOP, notesCmdEdit + op);
    ui::muted(add(h, L"STATIC", lang::text(L"在资源管理器切换到详细信息视图，右键列标题并勾选“备注”即可直接查看文件夹备注。"), SS_NOPREFIX, notesTip));
    add(h, L"BUTTON", lang::text(L"右键菜单管理 →"), WS_TABSTOP, notesOpenContextMenu);

    // DoubleClick detail page controls
    ui::muted(add(h, L"STATIC", lang::text(L"双击空白处执行的动作"), SS_NOPREFIX, dblClickLabel));
    dblClickParentRadio = add(h, L"BUTTON", lang::text(L"返回上一级文件夹"),
        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, dblClickParent);
    dblClickBackRadio = add(h, L"BUTTON", lang::text(L"后退到上一次浏览的位置"),
        BS_AUTORADIOBUTTON | WS_TABSTOP, dblClickBack);
    ui::muted(add(h, L"STATIC", lang::text(L"在资源管理器文件列表的空白处双击时触发，修改立即生效。"), SS_NOPREFIX, dblClickDesc));

    // QuickFolders detail page controls
    dialogCheck = add(h, L"BUTTON", lang::text(L"同时支持文件与文件夹选择对话框"), BS_AUTOCHECKBOX | WS_GROUP | WS_TABSTOP, 15);
    elevatedDialogCheck = add(h, L"BUTTON", lang::text(L"支持管理员权限软件的文件对话框（需管理员授权）"), BS_AUTOCHECKBOX | WS_TABSTOP, elevatedDialogToggle);
    ui::muted(add(h, L"STATIC", lang::text(L"桌面和资源管理器中均可使用，悬停展开子目录。选中项目后按中键支持复制或移动。"), SS_NOPREFIX, quickDesc));

    // Tabs detail page controls (Win10 only)
    preferNewTabCheck = nullptr;
    if (tabsSupported()) {
      ui::muted(add(h, L"STATIC", lang::text(L"悬停标签：滚轮切换，中键关闭；右键可新建和设置。升级后请重新打开文件夹窗口。"), SS_NOPREFIX, tabsDesc));
      preferNewTabCheck = add(h, L"BUTTON", lang::text(L"始终在新标签中打开文件夹"), BS_AUTOCHECKBOX | WS_TABSTOP, 21);
      ui::muted(add(h, L"STATIC", lang::text(L"首个窗口正常打开；桌面或其他软件另开文件夹时优先使用新标签；标签右键仍可开新窗口。"), SS_NOPREFIX, tabsNewTabDesc));
    }

    // Preferences (General) page controls
    ui::muted(add(h, L"STATIC", lang::text(L"外观主题"), 0, 32));
    const auto automaticUpdate = add(h, L"BUTTON", lang::text(L"自动更新"), BS_AUTOCHECKBOX | WS_TABSTOP,
                                     automaticUpdateCheck);
    SendMessageW(automaticUpdate, BM_SETCHECK, automaticUpdatesEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    ui::muted(add(h, L"STATIC", lang::text(L"开启后，打开设置时每天最多检查一次。通用版会自动安装无需管理员授权的更新。"),
                  SS_NOPREFIX, automaticUpdateDescription));
    ui::muted(add(h, L"STATIC", L"", SS_NOPREFIX, updateStatusLabel));
    updateControls(h);
    ui::muted(add(h, L"STATIC", lang::text(L"故障排查"), SS_NOPREFIX, diagnosticsLabel));
    ui::muted(add(h, L"STATIC", lang::text(L"遇到问题时可复制诊断信息发给开发者排查。"), SS_NOPREFIX, diagnosticsDesc));
    add(h, L"BUTTON", lang::text(L"复制诊断信息"), WS_TABSTOP, 13);

    // QuickFind detail page controls
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
    SendMessageW(themeChoice, CB_SETITEMHEIGHT, WPARAM(-1), scale(26));

    body = CreateWindowExW(WS_EX_CONTROLPARENT, L"ShibaKit.Settings.Content", lang::text(L"功能总览"),
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
      refreshNotesControls(settingsWindow);
      syncFeatureNavigation();
    });
    entriesPage = explorer_entries_page::create(h, *appearance, font);
    if (!managerPage || !contextPage || !entriesPage) return -1;

    auto makeCard = [&](size_t index, SettingsPage detailPage, int toggleId, int actionId,
                        const wchar_t *name, const wchar_t *desc, const wchar_t *actionLabel, bool visible = true) {
      auto &card = cards[index];
      card.detailPage = detailPage;
      card.visible = visible;
      if (!visible) return;
      card.toggle = add(body, L"BUTTON", lang::text(name), BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, toggleId);
      SendMessageW(card.toggle, WM_SETFONT, reinterpret_cast<WPARAM>(cardTitleFont), TRUE);
      ui::toggleSwitch(card.toggle);
      ui::surface(card.toggle);
      card.description = add(body, L"STATIC", lang::text(desc), SS_NOPREFIX, 111 + int(index));
      SendMessageW(card.description, WM_SETFONT, reinterpret_cast<WPARAM>(cardDescFont), TRUE);
      ui::muted(card.description);
      ui::surface(card.description);
      card.action = add(body, L"BUTTON", lang::text(actionLabel), BS_OWNERDRAW | WS_TABSTOP, actionId);
      ui::surface(card.action);
      SetWindowSubclass(card.toggle, cardChildProc, 2, static_cast<DWORD_PTR>(index));
      SetWindowSubclass(card.action, cardChildProc, 2, static_cast<DWORD_PTR>(index));
    };
    makeCard(0, SettingsPage::Notes, 11, cardEditNotes, L"文件夹备注",
        L"在文件夹右键菜单中添加、编辑和修复备注，关闭后保留已有备注。", L"编辑 →");
    makeCard(1, SettingsPage::DoubleClick, 12, cardEditDoubleClick, L"双击空白处导航",
        L"在资源管理器文件列表的空白处双击执行导航操作。", L"编辑 →");
    makeCard(2, SettingsPage::QuickFolders, 14, cardEditQuick, L"中键快速定位",
        L"桌面与资源管理器中按中键，快速访问置顶、历史并支持复制或移动。", L"编辑 →");
    makeCard(3, SettingsPage::BoxAppearance, desktopGroupsToggle, cardEditDesktopGroups, L"桌面盒子",
        L"在桌面用盒子分组收纳文件与文件夹，支持毛玻璃与样式自定义。", L"编辑 →");
    makeCard(4, SettingsPage::QuickFind, quickFindToggle, cardEditQuickFind, L"快捷搜索",
        L"键入名称或中文首字母，快速定位并打开文件或文件夹。", L"编辑 →");
    makeCard(5, SettingsPage::Tabs, 18, cardEditTabs, L"资源管理器多标签（Windows 10）",
        L"为 Windows 10 资源管理器增加标签栏，支持滚轮切换与中键关闭。", L"编辑 →", tabsSupported());

    notesCheck = cards[0].toggle;
    navigationCheck = cards[1].toggle;
    quickCheck = cards[2].toggle;
    desktopGroupsCheck = cards[3].toggle;
    quickFindCheck = cards[4].toggle;
    tabsCheck = tabsSupported() ? cards[5].toggle : nullptr;

    loadSettingsToControls(h);
    appearance->apply(h);
    quick::setManagerTheme(managerPage, appearance->mode);
    showPage(activePage);
    return 0;
  }
  case WM_COMMAND:
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
        reopenForLanguage = true;
        DestroyWindow(h);
      } catch (...) {
        SendDlgItemMessageW(h, languageChoice, CB_SETCURSEL, static_cast<unsigned>(lang::settings().choice), 0);
        MessageBoxW(h, id == languageImport ? lang::text(L"无法导入语言文件。请使用有效的 ShibaKit XML 语言模板。") :
            lang::text(L"无法保存语言设置或模板。"), L"ShibaKit", MB_OK | MB_ICONERROR);
      }
      return 0;
    }
    if ((LOWORD(w) == 11 || LOWORD(w) == 12 || LOWORD(w) == 14 ||
         LOWORD(w) == desktopGroupsToggle || LOWORD(w) == quickFindToggle || LOWORD(w) == 18) &&
        HIWORD(w) == BN_CLICKED) {
      handleFeatureToggle(h, LOWORD(w));
    } else if (LOWORD(w) >= notesCmdEdit && LOWORD(w) <= notesCmdRepair && HIWORD(w) == BN_CLICKED) {
      const int op = LOWORD(w) - notesCmdEdit;
      const bool hidden = SendDlgItemMessageW(h, LOWORD(w), BM_GETCHECK, 0, 0) != BST_CHECKED;
      try {
        setNoteCommandHidden(op, hidden);
        syncNoteMenus();
      } catch (const fn::Error &e) {
        refreshNotesControls(h);
        if (IsWindowVisible(h)) MessageBoxW(h, e.message.c_str(), L"ShibaKit", MB_OK | MB_ICONERROR);
      } catch (...) {
        refreshNotesControls(h);
        if (IsWindowVisible(h)) MessageBoxW(h, lang::text(L"无法更新备注菜单开关"), L"ShibaKit", MB_OK | MB_ICONERROR);
      }
    } else if (LOWORD(w) == notesOpenContextMenu && HIWORD(w) == BN_CLICKED) {
      showPage(SettingsPage::ContextMenus);
      SetFocus(GetDlgItem(h, 47));
    } else if ((LOWORD(w) == dblClickParent || LOWORD(w) == dblClickBack) && HIWORD(w) == BN_CLICKED) {
      commitSettings(h, CommitScope::Navigation);
    } else if (LOWORD(w) == 15 && HIWORD(w) == BN_CLICKED) {
      commitSettings(h, CommitScope::Quick);
    } else if (LOWORD(w) == elevatedDialogToggle && HIWORD(w) == BN_CLICKED && elevatedDialogCheck) {
      const bool wantEnabled = SendMessageW(elevatedDialogCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
      if (!IsWindowVisible(h)) {
        setElevatedDialogQuickEnabled(wantEnabled);
      } else {
        try {
          if (!quick::configureElevatedHelper(quick::executable(), wantEnabled)) {
            loadSettingsToControls(h);
          }
        } catch (const fn::Error &e) {
          loadSettingsToControls(h);
          MessageBoxW(h, e.message.c_str(), L"ShibaKit", MB_OK | MB_ICONERROR);
        } catch (...) {
          loadSettingsToControls(h);
          MessageBoxW(h, lang::text(L"操作未完成，请稍后重试。"), L"ShibaKit", MB_OK | MB_ICONERROR);
        }
      }
    } else if (LOWORD(w) == 21 && HIWORD(w) == BN_CLICKED && preferNewTabCheck) {
      commitSettings(h, CommitScope::Tabs);
    } else if (LOWORD(w) == 17 && HIWORD(w) == CBN_SELCHANGE) {
      auto selected = SendMessageW(themeChoice, CB_GETCURSEL, 0, 0);
      if (selected >= 0 && selected <= 2) {
        appearance->reset(ui::ThemeMode(selected));
        syncSidebarBrush();
        appearance->apply(h);
        quick::setManagerTheme(managerPage, appearance->mode);
        contextmenu::setTheme(contextPage, unsigned(appearance->mode));
        commitSettings(h, CommitScope::Theme);
        SendMessageW(entriesPage, WM_THEMECHANGED, 0, 0);
      }
    } else if (LOWORD(w) == desktopGroupsManage && HIWORD(w) == BN_CLICKED) {
      if (IsWindowEnabled(desktopGroupsManager) && !desktop_groups::showManager())
        MessageBoxW(h, lang::text(L"无法打开桌面盒子管理。"), L"ShibaKit", MB_OK | MB_ICONERROR);
    } else if ((LOWORD(w) == boxGlass || LOWORD(w) == boxShadow) && HIWORD(w) == BN_CLICKED) {
      boxStyle.glass = SendDlgItemMessageW(h, boxGlass, BM_GETCHECK, 0, 0) == BST_CHECKED;
      boxStyle.shadow = SendDlgItemMessageW(h, boxShadow, BM_GETCHECK, 0, 0) == BST_CHECKED;
      updateBoxStylePreview(h);
      commitSettings(h, CommitScope::BoxStyle);
    } else if (LOWORD(w) >= cardEditNotes && LOWORD(w) <= cardEditTabs && HIWORD(w) == BN_CLICKED) {
      const size_t cardIndex = static_cast<size_t>(LOWORD(w) - cardEditNotes);
      if (cardIndex < cards.size() && cards[cardIndex].visible && IsWindowEnabled(cards[cardIndex].action)) {
        const auto targetPage = cards[cardIndex].detailPage;
        showPage(targetPage);
        auto it = std::find_if(sidebarItems.begin(), sidebarItems.end(),
            [targetPage](const auto &entry) { return entry.page == targetPage; });
        if (it != sidebarItems.end()) SetFocus(GetDlgItem(h, it->id));
      }
    } else if (auto button = std::find_if(sidebarItems.begin(), sidebarItems.end(),
                   [id = int(LOWORD(w))](const auto &entry) { return entry.id == id; });
               button != sidebarItems.end() && HIWORD(w) == BN_CLICKED) {
      showPage(button->page);
    } else if (LOWORD(w) == 13 && HIWORD(w) == BN_CLICKED) {
      try { copyDiagnostics(h); } catch (const fn::Error &e) { MessageBoxW(h, e.message.c_str(), L"ShibaKit", MB_OK | MB_ICONERROR); }
    } else if ((HIWORD(w) == BN_CLICKED && std::any_of(std::begin(findToggles), std::end(findToggles),
                   [&](const auto &toggle) { return toggle.id == LOWORD(w); })) ||
               (HIWORD(w) == CBN_SELCHANGE && (LOWORD(w) == findFolderAction || LOWORD(w) == findFileAction ||
                                             LOWORD(w) == findPosition || LOWORD(w) == findRows))) {
      try {
        saveFindSettings(h);
      } catch (const fn::Error &error) {
        refreshFindSettings(h);
        MessageBoxW(h, error.message.c_str(), L"ShibaKit", MB_OK | MB_ICONERROR);
      }
    } else if (LOWORD(w) == automaticUpdateCheck && HIWORD(w) == BN_CLICKED && *app::updateRepository &&
               (!updateWork.valid() || workingAutomatically())) {
      const DWORD enabled = SendDlgItemMessageW(h, automaticUpdateCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
      if (RegSetKeyValueW(HKEY_CURRENT_USER, configKey, automaticUpdateValue, REG_DWORD, &enabled, sizeof(enabled)) != ERROR_SUCCESS) {
        SendDlgItemMessageW(h, automaticUpdateCheck, BM_SETCHECK, automaticUpdatesEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        MessageBoxW(h, lang::text(L"无法保存自动更新设置。"), lang::text(L"ShibaKit 更新"), MB_OK | MB_ICONERROR);
      } else if (enabled && IsWindowVisible(h)) {
        checkAutomatically(h);
      }
    } else if (LOWORD(w) == updateButton && HIWORD(w) == BN_CLICKED && !updateWork.valid()) {
      if (*app::updateRepository) startUpdateWork(h, UpdateOperation::Check);
      else MessageBoxW(h, lang::text(L"此构建尚未配置 GitHub 更新源。请使用 GitHub Releases 发布的安装包。"), lang::text(L"ShibaKit 更新"), MB_OK | MB_ICONINFORMATION);
    }
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
        if (LOWORD(w) == TB_THUMBTRACK) {
          previewBoxStyle(h, previous);
        } else {
          updateBoxStylePreview(h);
          commitSettings(h, CommitScope::BoxStyle);
        }
      }
    }
    return 0;
  case WM_MOUSEWHEEL: if (activePage == SettingsPage::Features) settingsScroll.wheel(GET_WHEEL_DELTA_WPARAM(w)); return 0;
  case WM_SIZE: if (body) layoutSettings(); return 0;
  case WM_GETMINMAXINFO: {
    auto *limits = reinterpret_cast<MINMAXINFO *>(l); limits->ptMinTrackSize = {scale(900), scale(500)}; return 0;
  }
  case WM_SETTINGCHANGE:
    if (l && _wcsicmp(reinterpret_cast<LPCWSTR>(l), L"ImmersiveColorSet") != 0) return 0;
    [[fallthrough]];
  case WM_THEMECHANGED:
    appearance->reset(appearance->mode); syncSidebarBrush(); appearance->apply(h); quick::setManagerTheme(managerPage, appearance->mode);
    SendMessageW(entriesPage, WM_THEMECHANGED, 0, 0); return 0;
  case WM_DPICHANGED: {
    UINT oldDpi = dpi; dpi = HIWORD(w); scrollPosition = MulDiv(scrollPosition, dpi, oldDpi);
    auto oldFont = font, oldSidebarActive = sidebarActiveFont, oldBrand = brandFont,
         oldHeading = headingFont, oldCardTitle = cardTitleFont, oldCardDesc = cardDescFont,
         oldCardIcon = cardIconFont, oldSmall = smallFont, oldSymbol = symbolFont;
    font = ui::makeFont(dpi, 14, FW_NORMAL);
    sidebarActiveFont = ui::makeFont(dpi, 14, FW_SEMIBOLD);
    brandFont = ui::makeFont(dpi, 18, FW_SEMIBOLD);
    headingFont = ui::makeFont(dpi, 18, FW_LIGHT);
    cardTitleFont = ui::makeFont(dpi, 14, FW_SEMIBOLD);
    cardDescFont = ui::makeFont(dpi, 12, FW_NORMAL);
    cardIconFont = ui::makeSymbolFont(dpi, 22);
    smallFont = ui::makeFont(dpi, 13, FW_NORMAL);
    symbolFont = ui::makeSymbolFont(dpi, 14);
    for (HWND child = GetWindow(h, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
      if (child != body && child != managerPage && child != contextPage && child != entriesPage)
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    }
    SendMessageW(GetDlgItem(h, 30), WM_SETFONT, reinterpret_cast<WPARAM>(headingFont), FALSE);
    SendMessageW(GetDlgItem(h, 35), WM_SETFONT, reinterpret_cast<WPARAM>(brandFont), FALSE);
    SendMessageW(GetDlgItem(h, 31), WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
    SendMessageW(GetDlgItem(h, featuresSubtitle), WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
    for (const auto &card : cards)
      if (card.visible) {
        if (card.toggle) SendMessageW(card.toggle, WM_SETFONT, reinterpret_cast<WPARAM>(cardTitleFont), FALSE);
        if (card.description) SendMessageW(card.description, WM_SETFONT, reinterpret_cast<WPARAM>(cardDescFont), FALSE);
        if (card.action) SendMessageW(card.action, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), FALSE);
      }
    for (auto combo : {themeChoice, GetDlgItem(h, languageChoice), GetDlgItem(h, findFolderAction),
         GetDlgItem(h, findFileAction), GetDlgItem(h, findPosition), GetDlgItem(h, findRows)}) {
      SendMessageW(combo, CB_SETITEMHEIGHT, WPARAM(-1), scale(26));
      for (int i = 0; i < SendMessageW(combo, CB_GETCOUNT, 0, 0); ++i) SendMessageW(combo, CB_SETITEMHEIGHT, i, scale(26));
    }
    DeleteObject(oldFont); DeleteObject(oldSidebarActive); DeleteObject(oldBrand);
    DeleteObject(oldHeading); DeleteObject(oldCardTitle); DeleteObject(oldCardDesc);
    DeleteObject(oldCardIcon); DeleteObject(oldSmall); DeleteObject(oldSymbol);
    auto *rect = reinterpret_cast<RECT *>(l);
    SetWindowPos(h, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(managerPage, WM_DPICHANGED_AFTERPARENT, 0, 0);
    SendMessageW(contextPage, WM_DPICHANGED_AFTERPARENT, 0, 0);
    SendMessageW(entriesPage, WM_DPICHANGED_AFTERPARENT, 0, 0);
    layoutSettings();
    InvalidateRect(h, nullptr, FALSE);
    return 0;
  }
  case WM_DESTROY:
    if (!reopenForLanguage) restoreBoxStylePreview();
    KillTimer(h, updateTimer);
    if (!workingAutomatically()) updateWork = {};
    updateTip = nullptr;
    PostQuitMessage(0);
    return 0;
  case WM_NCDESTROY:
    RemovePropW(h, app::settingsWindowTag);
    DeleteObject(font); DeleteObject(sidebarActiveFont); DeleteObject(brandFont);
    DeleteObject(headingFont); DeleteObject(cardTitleFont); DeleteObject(cardDescFont);
    DeleteObject(cardIconFont); DeleteObject(smallFont); DeleteObject(symbolFont);
    if (sidebarBrush) { DeleteObject(sidebarBrush); sidebarBrush = nullptr; }
    font = sidebarActiveFont = brandFont = headingFont = cardTitleFont = cardDescFont = cardIconFont = smallFont = symbolFont = nullptr;
    body = managerPage = contextPage = entriesPage = settingsWindow = nullptr;
    cards = {}; sidebarDividers.clear(); hoveredCard = -1; hoveredCardAction = -1; hoveredSidebarId = 0;
    notesCheck = navigationCheck = dblClickParentRadio = dblClickBackRadio = quickCheck = dialogCheck = elevatedDialogCheck =
        tabsCheck = preferNewTabCheck = desktopGroupsCheck = desktopGroupsManager = quickFindCheck = themeChoice = nullptr;
    break;
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
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"ShibaKit.Settings.Content"; RegisterClassW(&cls);
  cls.lpfnWndProc = settingsProc; cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SHIBAKIT));
  cls.lpszClassName = L"ShibaKit.Settings"; RegisterClassW(&cls);
  do {
    reopenForLanguage = false;
    appearance = std::make_unique<ui::Theme>(); dpi = GetDpiForSystem();
    const RECT work = ui::mouseWorkArea();
    HWND window = CreateWindowExW(0, cls.lpszClassName, lang::text(L"ShibaKit 系统工具设置"),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        work.left, work.top, scale(980), scale(660), nullptr, nullptr, instance, nullptr);
    if (!window) {
      restoreBoxStylePreview();
      appearance.reset();
      return 1;
    }
    ui::centerLogical(window, work, 980, 660); ShowWindow(window, SW_SHOW);
    SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    SetFocus(GetDlgItem(window, sidebarItems[0].id));
    if (workingAutomatically()) SetTimer(window, updateTimer, 100, nullptr);
    else checkAutomatically(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == WM_KEYDOWN && (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN)) {
        HWND dropped = nullptr;
        for (auto combo : {themeChoice, GetDlgItem(window, languageChoice),
             GetDlgItem(window, findFolderAction), GetDlgItem(window, findFileAction), GetDlgItem(window, findPosition), GetDlgItem(window, findRows),
             explorer_entries_page::locationControl(entriesPage)})
          if (SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0)) dropped = combo;
        if (dropped) { SendMessageW(dropped, CB_SHOWDROPDOWN, FALSE, 0); continue; }
        if (message.wParam == VK_ESCAPE) SendMessageW(window, WM_CLOSE, 0, 0);
        else {
          HWND focused = GetFocus(); wchar_t clsName[32]{}; GetClassNameW(focused, clsName, ARRAYSIZE(clsName));
          if (!_wcsicmp(clsName, L"BUTTON")) SendMessageW(focused, BM_CLICK, 0, 0);
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
