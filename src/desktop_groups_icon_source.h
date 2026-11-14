#pragma once
#include "desktop_groups_model.h"

namespace desktop_groups {
struct DesktopIconSnapshot {
  std::vector<LooseItem> items;
  UINT dpi = 96;
  int iconSize = 0;
  SIZE spacing{};
  LOGFONTW labelFont{};
};

// Reads live Explorer layout. Positions are screen-space cell centers and sizes are in pixels.
std::optional<DesktopIconSnapshot> desktopIconPositions();
bool desktopSnapToGrid();
// Enumerates desktop paths independently of the visibility of Explorer's icon view.
std::optional<std::vector<std::wstring>> desktopIconPaths();
}
