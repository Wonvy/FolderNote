#pragma once
#include <windows.h>
#include <functional>
#include "quick_find_popup.h"
namespace quick_find {
// Attaches only to a same-thread standard ListBox or ListView owned by FolderNote.
void attach(HWND list, std::function<void(int)> openFolder = {}, std::function<CandidateIcon(int)> icon = {});
bool shortcutModifiers();
}
