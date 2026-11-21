#pragma once
#include "desktop_groups_wallpaper.h"
#include <span>
namespace desktop_groups {
struct GlassArea { RECT bounds; int diameter; int strength = defaultBlurStrength; };
class DesktopGlass {
public:
  ~DesktopGlass();
  void clear();
  // Areas ordered front to back. A negative strength only occludes lower boxes.
  bool update(HWND host, std::shared_ptr<const WallpaperFrame> image, std::span<const GlassArea> areas,
              int strength = defaultBlurStrength);
  HWND handle() const { return shown ? window : nullptr; }
private:
  HWND window = nullptr;
  bool shown = false;
  std::shared_ptr<const WallpaperFrame> frame;
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
};
}
