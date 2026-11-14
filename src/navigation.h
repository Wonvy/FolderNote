#pragma once
#include <windows.h>
#include <string>
#include "theme.h"
namespace navigation {
enum class Action : DWORD { Off = 0, Parent = 1, Back = 2 };
struct Click {
  POINT point{};
  DWORD time{};
  HWND root{};
  HWND view{};
  bool blank = false;
};
bool isDoubleClick(const Click &first, const Click &second, UINT interval, int width, int height);
Action configuredAction();
bool notesEnabled();
bool noteCommandHidden(int operation);
void setNoteCommandHidden(int operation, bool hidden);
bool quickEnabled();
bool dialogQuickEnabled();
bool tabsSupported();
bool tabsEnabled();
bool preferNewTabEnabled();
struct TabsActivation { DWORD windows = 0, requested = 0, visible = 0; HRESULT result = S_FALSE; };
TabsActivation syncTabs();
Action preferredAction();
void saveSettings(bool notes, Action action, Action preferred);
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs);
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs, ui::ThemeMode theme);
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs, ui::ThemeMode theme, bool tabs);
void saveSettings(bool notes, Action action, Action preferred, bool quick, bool dialogs, ui::ThemeMode theme, bool tabs, bool preferNewTab);
void syncNoteMenus();
std::wstring diagnostics();
void stopLegacy();
void stop();
void reload();
enum class SettingsPage { Features, QuickFolders, ContextMenus, ExplorerEntries, Preferences, BoxAppearance };
int settings(HINSTANCE instance, SettingsPage page = SettingsPage::Features);
} // namespace navigation
