#pragma once

#include "desktop_groups_model.h"
#include <functional>
#include <memory>

namespace desktop_groups {
struct DesktopIconEvents {
  // The first path is the icon under the pointer. The handler owns the icon copy.
  std::function<void(HWND, std::vector<std::wstring>, HICON, RECT, POINT)> beginDrag;
  std::function<void(HWND, const std::wstring &, POINT)> contextMenu;
  std::function<void(int)> zoom;
  std::function<void(POINT)> dragReleased;
  std::function<void(POINT)> desktopMenuOpened;
};

struct DesktopIconMetrics {
  int iconSize = 32;
  SIZE spacing{92, 78};
  UINT dpi = 96;
  LOGFONTW labelFont{};
};

class DesktopIcons {
public:
  DesktopIcons(HINSTANCE instance, DesktopIconEvents events);
  ~DesktopIcons();
  HWND window() const;
  std::wstring itemAt(POINT screen) const;
  std::vector<LooseItem> positions() const;
  // The points are physical screen coordinates of the icon cells' centers.
  bool update(const std::vector<LooseItem> &items, DesktopIconMetrics metrics = {});

private:
  struct State;
  std::unique_ptr<State> state;
};
}
