#pragma once
#include "theme.h"
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace quick::popup {
enum class Symbol { Folder, Desktop, Downloads, Documents, History, Star, Unpin, Settings, Trash, Window, More, Copy, Move, Visibility, AddFolder };
enum class Row { Action, Heading, Separator, Hint, Toolbar, Tabs };
struct IconAction {
  unsigned command; Symbol symbol; std::wstring title;
  bool enabled = true, checked = false;
  std::wstring text; // Nonempty for a text button instead of an icon.
};
struct Slider {
  int minimum = 0, maximum = 100, value = 0;
  std::function<void(int)> preview;
  std::function<bool(int)> commit;
  bool hue = false;
  bool showUnit = true;
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
  bool checked = false;
  std::optional<Slider> slider;
};
struct Branch {
  std::vector<Item> items;
  // Nonblocking: returns true when the asynchronous folder listing is ready.
  std::function<bool(Branch &)> poll;
  // Return true after updating this root in place to keep the same popup open.
  std::function<bool(unsigned)> invoke;
  bool compact = false;
};
unsigned show(HINSTANCE instance, POINT point, std::shared_ptr<Branch> root, ui::ThemeMode theme, HWND anchor);
}
