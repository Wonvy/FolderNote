#pragma once
#include <string>
#include <windows.h>

namespace desktop_groups {
bool showItemMenu(HWND owner, const std::wstring &path, POINT point, bool canRemove, bool &remove, bool &rename);
}
