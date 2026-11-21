#pragma once
#include <windows.h>
#include <functional>
#include "quick_find_popup.h"
namespace quick_find {
// Attaches only to a same-thread standard ListBox or ListView owned by FolderNote.
// Activation callbacks select/open according to settings().opens for their actual item type.
void attach(HWND list, std::function<void(int)> activate = {}, std::function<CandidateIcon(int)> icon = {});
bool shortcutModifiers();
}
