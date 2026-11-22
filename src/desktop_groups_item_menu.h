#pragma once
#include <span>
#include <string>
#include <windows.h>

namespace desktop_groups {
struct ItemMenuResult {
  bool shown = false;
  bool remove = false;
  bool rename = false;
  bool deleteItem = false;
  bool permanentDelete = false;
  bool invokedShellCommand = false;
};

ItemMenuResult showItemMenu(HWND owner, std::span<const std::wstring> paths, POINT point, bool canRemove);
}
