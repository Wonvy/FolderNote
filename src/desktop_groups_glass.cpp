#include "desktop_groups_glass.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;
using namespace Gdiplus;
using ShellString = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>;

int width(const RECT &rect) { return rect.right - rect.left; }
int height(const RECT &rect) { return rect.bottom - rect.top; }

Color color(COLORREF rgb, BYTE alpha = 255) {
  return Color(alpha, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

bool effectsEnabled() {
  HIGHCONTRASTW contrast{sizeof(contrast)};
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
      (contrast.dwFlags & HCF_HIGHCONTRASTON)) return false;

  DWORD value = 1, size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS || value != 0;
}

void blur(Bitmap &image, int columns, int rows) {
  constexpr int radius = 3;
  constexpr int diameter = radius * 2 + 1;
  std::vector<ARGB> horizontal(static_cast<size_t>(columns) * rows);
  const Rect area(0, 0, columns, rows);
  BitmapData data{};
  if (image.LockBits(&area, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppRGB, &data) != Ok) return;

  const auto row = [&](int y) {
    return reinterpret_cast<ARGB *>(static_cast<BYTE *>(data.Scan0) + static_cast<ptrdiff_t>(data.Stride) * y);
  };
  const auto average = [](const auto &pixel) {
    unsigned r = 0, g = 0, b = 0;
    for (int offset = -radius; offset <= radius; ++offset) {
      const auto value = pixel(offset);
      r += (value >> 16) & 255;
      g += (value >> 8) & 255;
      b += value & 255;
    }
    return 0xff000000u | ((r / diameter) << 16) | ((g / diameter) << 8) | (b / diameter);
  };
  for (int y = 0; y < rows; ++y) {
    const auto line = row(y);
    auto out = horizontal.data() + static_cast<size_t>(y) * columns;
    for (int x = 0; x < columns; ++x)
      out[x] = average([&](int offset) { return line[std::clamp(x + offset, 0, columns - 1)]; });
  }
  for (int y = 0; y < rows; ++y) {
    const auto line = row(y);
    for (int x = 0; x < columns; ++x) {
      line[x] = average([&](int offset) {
        return horizontal[static_cast<size_t>(std::clamp(y + offset, 0, rows - 1)) * columns + x];
      });
    }
  }
  image.UnlockBits(&data);
}

std::unique_ptr<Bitmap> makeWallpaper(const wchar_t *path, const RECT &screen,
                                      DESKTOP_WALLPAPER_POSITION position, COLORREF background, bool glass) {
  Bitmap original(path);
  const auto sourceWidth = static_cast<float>(original.GetWidth());
  const auto sourceHeight = static_cast<float>(original.GetHeight());
  if (original.GetLastStatus() != Ok || sourceWidth <= 0 || sourceHeight <= 0) return {};

  const int pixelsWide = width(screen), pixelsHigh = height(screen);
  // The reduced bitmap both bounds memory and makes the blur independent of source image size.
  const int sample = glass ? std::max(10, (std::max(pixelsWide, pixelsHigh) + 1023) / 1024) : 1;
  const int columns = (pixelsWide + sample - 1) / sample;
  const int rows = (pixelsHigh + sample - 1) / sample;
  auto image = std::make_unique<Bitmap>(columns, rows, PixelFormat32bppRGB);
  if (image->GetLastStatus() != Ok) return {};

  const auto scaleX = static_cast<float>(columns) / static_cast<float>(pixelsWide);
  const auto scaleY = static_cast<float>(rows) / static_cast<float>(pixelsHigh);
  {
    Graphics graphics(image.get());
    graphics.Clear(color(background));
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
    Status result = Ok;
    if (position == DWPOS_TILE) {
      TextureBrush tile(&original, WrapModeTile);
      tile.ScaleTransform(scaleX, scaleY, MatrixOrderAppend);
      tile.TranslateTransform(-static_cast<float>(screen.left) * scaleX,
                              -static_cast<float>(screen.top) * scaleY, MatrixOrderAppend);
      result = graphics.FillRectangle(&tile, 0, 0, columns, rows);
    } else {
      auto targetWidth = static_cast<float>(pixelsWide), targetHeight = static_cast<float>(pixelsHigh);
      if (position != DWPOS_STRETCH) {
        float scale = 1.0f;
        if (position == DWPOS_FIT) scale = std::min(targetWidth / sourceWidth, targetHeight / sourceHeight);
        if (position == DWPOS_FILL || position == DWPOS_SPAN)
          scale = std::max(targetWidth / sourceWidth, targetHeight / sourceHeight);
        targetWidth = sourceWidth * scale;
        targetHeight = sourceHeight * scale;
      }
      const RectF destination((static_cast<float>(columns) - targetWidth * scaleX) / 2.0f,
                              (static_cast<float>(rows) - targetHeight * scaleY) / 2.0f,
                              targetWidth * scaleX, targetHeight * scaleY);
      ImageAttributes attributes;
      attributes.SetWrapMode(WrapModeTileFlipXY);
      result = graphics.DrawImage(&original, destination, 0, 0, sourceWidth, sourceHeight, UnitPixel, &attributes);
    }
    if (result != Ok) return {};
  }
  if (glass) blur(*image, columns, rows);
  return image;
}

struct Wallpaper {
  RECT bounds;
  std::unique_ptr<Bitmap> image;
};
}

struct GlassBackground::State {
  ULONG_PTR graphicsToken = 0;
  bool loaded = false;
  bool blurred = false;
  std::vector<Wallpaper> wallpapers;

  ~State() {
    wallpapers.clear();
    if (graphicsToken) GdiplusShutdown(graphicsToken);
  }

  void load(bool glass) {
    loaded = true;
    blurred = glass;
    wallpapers.clear();
    if (!effectsEnabled()) return;

    if (!graphicsToken) {
      GdiplusStartupInput input;
      if (GdiplusStartup(&graphicsToken, &input, nullptr) != Ok) return;
    }
    ComPtr<IDesktopWallpaper> desktop;
    if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&desktop)))) return;

    DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL;
    COLORREF background = GetSysColor(COLOR_BACKGROUND);
    desktop->GetPosition(&position);
    desktop->GetBackgroundColor(&background);
    RECT span{};
    if (position == DWPOS_SPAN) {
      const auto left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
      span = {left, top, left + GetSystemMetrics(SM_CXVIRTUALSCREEN), top + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
      add(desktop.Get(), nullptr, span, position, background);
      if (!wallpapers.empty()) return;
    }

    UINT count = 0;
    if (FAILED(desktop->GetMonitorDevicePathCount(&count))) return;
    for (UINT index = 0; index < count; ++index) {
      PWSTR raw = nullptr;
      const auto result = desktop->GetMonitorDevicePathAt(index, &raw);
      const ShellString monitor(raw, CoTaskMemFree);
      RECT bounds{};
      if (result != S_OK || !monitor || desktop->GetMonitorRECT(monitor.get(), &bounds) != S_OK) continue;
      // A slideshow can require a monitor ID even when one image spans the desktop.
      add(desktop.Get(), monitor.get(), position == DWPOS_SPAN ? span : bounds, position, background);
      if (position == DWPOS_SPAN && !wallpapers.empty()) return;
    }
  }

  void add(IDesktopWallpaper *desktop, const wchar_t *monitor, const RECT &bounds,
           DESKTOP_WALLPAPER_POSITION position, COLORREF background) {
    if (width(bounds) <= 0 || height(bounds) <= 0) return;
    PWSTR raw = nullptr;
    const auto result = desktop->GetWallpaper(monitor, &raw);
    const ShellString path(raw, CoTaskMemFree);
    if (result != S_OK || !path || !*path) return;
    if (auto image = makeWallpaper(path.get(), bounds, position, background, blurred))
      wallpapers.push_back({bounds, std::move(image)});
  }
};

GlassBackground::GlassBackground() : state(std::make_unique<State>()) {}
GlassBackground::~GlassBackground() = default;

void GlassBackground::reset() {
  state->loaded = false;
  state->wallpapers.clear();
}

void GlassBackground::paint(HWND window, HDC dc, RECT client, COLORREF tint, BYTE opacity, bool glass, POINT offset) {
  OffsetRect(&client, offset.x, offset.y);
  const auto previousColor = SetDCBrushColor(dc, tint);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  SetDCBrushColor(dc, previousColor);
  if (opacity == 255) return;
  if (!state->loaded || state->blurred != glass) state->load(glass);
  if (state->wallpapers.empty()) {
    if (!effectsEnabled()) return;
    Graphics graphics(dc);
    const auto mix = [opacity](BYTE a, BYTE b) { return static_cast<BYTE>((a * opacity + b * (255 - opacity)) / 255); };
    const auto desktop = GetSysColor(COLOR_BACKGROUND);
    const auto shade = RGB(mix(GetRValue(tint), GetRValue(desktop)),
                           mix(GetGValue(tint), GetGValue(desktop)), mix(GetBValue(tint), GetBValue(desktop)));
    const Rect area(client.left, client.top, width(client), height(client));
    if (!glass) {
      const SolidBrush background(color(shade));
      graphics.FillRectangle(&background, area);
    } else if (area.Width > 0 && area.Height > 0) {
      LinearGradientBrush wash(area, color(tint), color(shade), LinearGradientModeForwardDiagonal);
      graphics.FillRectangle(&wash, area);
    }
    return;
  }

  POINT origin{};
  if (!ClientToScreen(window, &origin)) return;
  origin.x -= offset.x;
  origin.y -= offset.y;
  Graphics graphics(dc);
  graphics.SetInterpolationMode(InterpolationModeHighQualityBilinear);
  graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
  graphics.SetClip(Rect(client.left, client.top, width(client), height(client)), CombineModeIntersect);
  ImageAttributes attributes;
  attributes.SetWrapMode(WrapModeTileFlipXY);
  const SolidBrush overlay(color(tint, opacity));
  for (const auto &wallpaper : state->wallpapers) {
    const auto &bounds = wallpaper.bounds;
    RECT local = bounds;
    OffsetRect(&local, -origin.x, -origin.y);
    RECT visible{};
    if (!IntersectRect(&visible, &local, &client)) continue;
    const Rect destination(local.left, local.top, width(local), height(local));
    graphics.DrawImage(wallpaper.image.get(), destination, 0, 0, static_cast<int>(wallpaper.image->GetWidth()),
                       static_cast<int>(wallpaper.image->GetHeight()), UnitPixel, &attributes);
    graphics.FillRectangle(&overlay, visible.left, visible.top, width(visible), height(visible));
  }
}
}
