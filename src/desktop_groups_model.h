#pragma once
#include <windows.h>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace desktop_groups {
inline constexpr std::size_t groupNameLimit = 120;
inline constexpr int defaultIconSize = 32;
inline constexpr std::array iconSizes{24, defaultIconSize, 48, 64, 96};
inline constexpr int maxItemSpacing = 24;

struct Tab {
  std::wstring name;
  std::wstring folder;
  std::vector<std::wstring> paths;
  // A stored null uses the theme color. Missing information inherits the host's tint on detach.
  std::optional<std::optional<COLORREF>> detachedTint;
};

enum class View { Icons, List };
enum class Sort { Manual, Name, Type, Modified };
enum class TitleStyle { Text, Icon, IconAndText };
enum class DisplayMode { Scroll, Pages };

struct Group {
  std::wstring name;
  RECT bounds{};
  std::vector<std::wstring> paths;
  bool collapsed = false;
  std::vector<Tab> tabs;
  View view = View::Icons;
  Sort sort = Sort::Manual;
  int iconSize = defaultIconSize;
  std::optional<int> opacity;
  std::optional<COLORREF> tint;
  int titleHeight = 36;
  TitleStyle titleStyle = TitleStyle::Text;
  std::wstring titleIcon;
  DisplayMode displayMode = DisplayMode::Scroll;
  int itemSpacing = 0;
};

struct LooseItem {
  std::wstring path;
  POINT position{};
};

struct Layout {
  std::vector<Group> groups;
  std::vector<LooseItem> looseItems;
};

std::filesystem::path layoutPath();
Layout loadLayout(const std::filesystem::path &file = layoutPath());
void saveLayout(const Layout &layout, const std::filesystem::path &file = layoutPath());
std::vector<Group> load(const std::filesystem::path &file = layoutPath());
void save(const std::vector<Group> &groups, const std::filesystem::path &file = layoutPath());
std::vector<std::wstring> desktopRoots();
bool samePath(const std::wstring &a, const std::wstring &b);
// Returns an existing, direct child of the personal or public desktop.
std::wstring desktopItem(const std::vector<std::wstring> &roots, const std::wstring &path);
// Tab 0 is the default desktop list. Additional tabs are indexed from 1.
// Assigns a desktop item to exactly one manual tab and returns whether it changed.
bool addItem(std::vector<Group> &groups, std::size_t target, const std::wstring &path, std::size_t tab = 0);
// Runtime group 0 is the synthetic desktop and cannot be merged or detached.
bool mergeGroups(std::vector<Group> &groups, std::size_t source, std::size_t target);
bool detachTab(std::vector<Group> &groups, std::size_t group, std::size_t tabIndex, RECT bounds);
}
