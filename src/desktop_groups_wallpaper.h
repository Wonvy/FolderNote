#pragma once
#include "desktop_groups_model.h"
#include <windows.h>
#include <shobjidl.h>
#include <memory>
#include <vector>

namespace desktop_groups {
struct WallpaperFrame {
  RECT screen{};
  int width = 0, height = 0;
  std::vector<DWORD> pixels; // Opaque BGRA, reduced-resolution desktop wallpaper.
};
struct WallpaperPlacement { double left, top, width, height; };
WallpaperPlacement placeWallpaper(SIZE image, RECT screen, DESKTOP_WALLPAPER_POSITION position);
void blurWallpaper(WallpaperFrame &frame, int radius);
int wallpaperBlurRadius(const WallpaperFrame &frame, int strength);
// UI-thread cache. File decoding/blur happens on a worker; movement never reads files.
std::shared_ptr<const WallpaperFrame> wallpaperBackdrop(int strength = defaultBlurStrength);
void invalidateWallpaperBackdrop();
}
