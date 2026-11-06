#pragma once
#include "theme.h"
#include <functional>
#include <memory>
#include <vector>

namespace quick::popup {
enum class Symbol { Folder, Desktop, Downloads, Documents, History, Star, Unpin, Settings, Trash, Window, More };
enum class Row { Action, Heading, Separator, Hint, Toolbar };
struct IconAction { unsigned command; Symbol symbol; std::wstring title; };
struct Branch;
struct Item {
  std::wstring title;
  unsigned command = 0;
  Symbol symbol = Symbol::Folder;
  std::shared_ptr<Branch> children;
  Row row = Row::Action;
  std::wstring iconPath; // The real shell item; blank for menu action icons.
  std::vector<IconAction> actions;
};
struct Branch {
  std::vector<Item> items;
  // Nonblocking: returns true when the asynchronous folder listing is ready.
  std::function<bool(Branch &)> poll;
};
unsigned show(HINSTANCE instance, POINT point, std::shared_ptr<Branch> root, ui::ThemeMode theme, HWND anchor);
}
