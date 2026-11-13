#pragma once
#include "theme.h"
#include <functional>
#include <memory>
#include <vector>

namespace quick::popup {
enum class Symbol { Folder, Desktop, Downloads, Documents, History, Star, Unpin, Settings, Trash, Window, More, Copy, Move };
enum class Row { Action, Heading, Separator, Hint, Toolbar, Tabs };
struct IconAction {
  unsigned command; Symbol symbol; std::wstring title;
  bool enabled = true, checked = false;
  std::wstring text; // Nonempty for a text button instead of an icon.
};
struct Branch;
struct Item {
  std::wstring title;
  unsigned command = 0;
  Symbol symbol = Symbol::Folder;
  std::shared_ptr<Branch> children;
  Row row = Row::Action;
  std::wstring iconPath; // The real shell item; blank for menu action icons.
  std::vector<IconAction> actions;
  unsigned leadingActions = 0; // Left-aligned buttons; remaining actions stay on the right.
};
struct Branch {
  std::vector<Item> items;
  // Nonblocking: returns true when the asynchronous folder listing is ready.
  std::function<bool(Branch &)> poll;
  // Return true after updating this root in place to keep the same popup open.
  std::function<bool(unsigned)> invoke;
};
unsigned show(HINSTANCE instance, POINT point, std::shared_ptr<Branch> root, ui::ThemeMode theme, HWND anchor);
}
