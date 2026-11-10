#pragma once
#include "theme.h"
#include <windows.h>
#include <string>
#include <vector>
#include <shobjidl.h>
namespace quick {
inline constexpr size_t historyLimit = 20, pinLimit = 64;
struct State { std::vector<std::wstring> pins, history; };
enum class MenuMode : unsigned { Navigate, Copy, Move };
// Canonicalization is lexical only: never touch a disk/network during menu setup.
std::wstring normalize(const std::wstring &path);
bool same(const std::wstring &, const std::wstring &);
State load();
void visit(const std::wstring &path);
void visitAsync(const std::wstring &path) noexcept;
void pin(const std::wstring &path, bool enabled);
void movePin(const std::wstring &path, int direction);
void clearHistory();
std::wstring viewPath(IShellView *view);
std::wstring executable();
void launchMenu(HWND target, HWND view, POINT point);
int menu(HINSTANCE, HWND target, HWND view, POINT point);
int manage(HINSTANCE, HWND owner = nullptr, ui::ThemeMode theme = ui::configuredTheme());
HWND createManagerPage(HINSTANCE, HWND parent, ui::ThemeMode);
void setManagerTheme(HWND, ui::ThemeMode);
void refreshManagerPage(HWND);
// The helper handles the desktop whenever Quick Folders is enabled, and
// Explorer/file dialogs only when file-dialog support is also enabled.
void syncHelper(const std::wstring &exe);
void configureStartup(const std::wstring &exe, bool enabled);
void stopHelper(bool removeStartup = true);
int helper(HINSTANCE);
bool isFileDialog(HWND);
bool isExplorer(HWND);
bool isDesktop(HWND);
bool addressEdit(HWND edit, HWND dialog);
}
