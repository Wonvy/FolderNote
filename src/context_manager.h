#pragma once
#include <windows.h>

namespace contextmenu {
HWND createPage(HINSTANCE instance, HWND owner, void (*setNoteHidden)(int, bool));
void setTheme(HWND page, unsigned mode);
}
