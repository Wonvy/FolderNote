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
inline constexpr int defaultBlurStrength = 25;

struct Tab {
  std::wstring name;
  std::wstring folder;
  std::vector<std::wstring> paths;
  // A stored null uses the theme color. Missing information inherits the host's tint on detach.
  std::optional<std::optional<COLORREF>> detachedTint;
};

enum class View { Icons, List };
enum class Sort { Manual, Name, Type, Modified };
enum class TitleStyle { Text = 0, Icon = 1, IconAndText = 2, Hidden = 3 };
enum class DisplayMode { Scroll, Pages };

constexpr bool showsTitleIcon(TitleStyle style) {
  return style == TitleStyle::Icon || style == TitleStyle::IconAndText;
}

constexpr bool showsTitleText(TitleStyle style) {
  return style == TitleStyle::Text || style == TitleStyle::IconAndText;
}

constexpr TitleStyle titleStyleFor(bool icon, bool text) {
  if (icon) return text ? TitleStyle::IconAndText : TitleStyle::Icon;
  return text ? TitleStyle::Text : TitleStyle::Hidden;
}

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
  std::optional<bool> glass;
  int blurStrength = defaultBlurStrength;
  bool autoCollapse = false;
  std::wstring folder;
  // The first tab can carry a tint from another box without changing this box's appearance.
  std::optional<std::optional<COLORREF>> primaryDetachedTint;
};

struct LooseItem {
  std::wstring path;
  POINT position{};
  // Older layouts used the native grid, so zero represents its spacing.
  SIZE spacing{};
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
bool isDesktopShellItem(const std::wstring &path);
// Returns an existing, direct child of the personal or public desktop.
std::wstring desktopItem(const std::vector<std::wstring> &roots, const std::wstring &path);
// A box's main tab is 0. Additional tabs are indexed from 1.
const std::wstring &folderForTab(const Group &group, std::size_t tab);
// Assigns a desktop item to exactly one manual tab and returns whether it changed.
bool addItem(std::vector<Group> &groups, std::size_t target, const std::wstring &path, std::size_t tab = 0);
// Group indices are zero-based. A single-tab box cannot be split.
bool mergeGroups(std::vector<Group> &groups, std::size_t source, std::size_t target);
bool detachTab(std::vector<Group> &groups, std::size_t group, std::size_t tabIndex, RECT bounds);
// Insertion is a slot in the target's original header, from 0 to tabs.size() + 1.
bool moveTab(std::vector<Group> &groups, std::size_t source, std::size_t tab,
             std::size_t target, std::size_t insertion);
}
