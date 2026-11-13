#pragma once
#include "theme.h"

namespace explorer_entries_page {
HWND create(HWND owner, ui::Theme &theme, HFONT font);
void refresh(HWND page);
HWND locationControl(HWND page);
HWND viewControl(HWND page, bool icons);
}
