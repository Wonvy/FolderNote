#include "desktop_groups_wallpaper.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <deque>
#include <map>
#include <string>
#include <thread>

namespace desktop_groups {
using Microsoft::WRL::ComPtr;
WallpaperPlacement placeWallpaper(SIZE image, RECT screen, DESKTOP_WALLPAPER_POSITION position) {
  const double width = screen.right - screen.left, height = screen.bottom - screen.top;
  if (image.cx <= 0 || image.cy <= 0 || width <= 0 || height <= 0) return {};
  if (position == DWPOS_STRETCH) return {double(screen.left), double(screen.top), width, height};
  const double scale = position == DWPOS_CENTER || position == DWPOS_TILE ? 1 : position == DWPOS_FIT
      ? std::min(width / image.cx, height / image.cy) : std::max(width / image.cx, height / image.cy);
  return {screen.left + (width - image.cx * scale) / 2, screen.top + (height - image.cy * scale) / 2,
          image.cx * scale, image.cy * scale};
}
void blurWallpaper(WallpaperFrame &frame, int radius) {
  if (radius <= 0) return;
  if (frame.width <= 0 || frame.height <= 0 || frame.pixels.size() != size_t(frame.width) * frame.height) return;
  radius = std::clamp(radius, 1, 32);
  std::vector<DWORD> scratch(frame.pixels.size());
  const auto pass = [&](bool vertical) {
    const int length = vertical ? frame.height : frame.width, lines = vertical ? frame.width : frame.height;
    const auto index = [&](int line, int p) { return vertical ? size_t(p) * frame.width + line : size_t(line) * frame.width + p; };
    const int count = radius * 2 + 1;
    for (int line = 0; line < lines; ++line) {
      int red = 0, green = 0, blue = 0;
      const auto add = [&](int p, int sign) {
        const auto color = frame.pixels[index(line, std::clamp(p, 0, length - 1))];
        red += sign * int((color >> 16) & 255); green += sign * int((color >> 8) & 255); blue += sign * int(color & 255);
      };
      for (int p = -radius; p <= radius; ++p) add(p, 1);
      for (int p = 0; p < length; ++p) {
        scratch[index(line, p)] = 0xff000000 | ((red / count) << 16) | ((green / count) << 8) | (blue / count);
        add(p - radius, -1); add(p + radius + 1, 1);
      }
    }
    frame.pixels.swap(scratch);
  };
  for (int i = 0; i < 3; ++i) { pass(false); pass(true); }
}
int wallpaperBlurRadius(const WallpaperFrame &frame, int strength) {
  if (strength <= 0 || frame.width <= 0 || frame.screen.right <= frame.screen.left) return 0;
  const double step = double(frame.screen.right - frame.screen.left) / frame.width;
  return std::clamp(int(std::lround(std::clamp(strength, 0, 100) * 0.64 / step)), 1, 32);
}
namespace {
struct Image { SIZE original{}; UINT width = 0, height = 0; std::vector<DWORD> pixels; };
Image decode(IWICImagingFactory *factory, const std::wstring &path) {
  Image result;
  ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler; ComPtr<IWICFormatConverter> converter;
  UINT width = 0, height = 0;
  if (path.empty() || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnLoad, &decoder)) || FAILED(decoder->GetFrame(0, &frame)) ||
      FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 100000 || height > 100000) return result;
  result.original = {LONG(width), LONG(height)};
  const double scale = std::min(1.0, 2048.0 / std::max(width, height));
  result.width = std::max(1U, UINT(std::ceil(width * scale)));
  result.height = std::max(1U, UINT(std::ceil(height * scale)));
  if (FAILED(factory->CreateBitmapScaler(&scaler)) ||
      FAILED(scaler->Initialize(frame.Get(), result.width, result.height, WICBitmapInterpolationModeFant)) ||
      FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone,
                                  nullptr, 0, WICBitmapPaletteTypeCustom))) return {};
  result.pixels.resize(size_t(result.width) * result.height);
  if (FAILED(converter->CopyPixels(nullptr, result.width * 4, UINT(result.pixels.size() * 4),
      reinterpret_cast<BYTE *>(result.pixels.data())))) return {};
  return result;
}
std::shared_ptr<const WallpaperFrame> render() {
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return {};
  struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
  ComPtr<IDesktopWallpaper> wallpaper; ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper))) ||
      FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return {};
  struct Monitor { RECT bounds; std::wstring path; };
  std::vector<Monitor> monitors;
  UINT count = 0; wallpaper->GetMonitorDevicePathCount(&count);
  if (!count || count > 32) return {};
  RECT desktop{};
  for (UINT i = 0; i < count; ++i) {
    LPWSTR id = nullptr, path = nullptr; RECT bounds{};
    if (FAILED(wallpaper->GetMonitorDevicePathAt(i, &id))) continue;
    const auto result = wallpaper->GetMonitorRECT(id, &bounds);
    wallpaper->GetWallpaper(id, &path);
    if (SUCCEEDED(result) && bounds.right > bounds.left && bounds.bottom > bounds.top) {
      monitors.push_back({bounds, path ? path : L""}); UnionRect(&desktop, &desktop, &bounds);
    }
    CoTaskMemFree(path); CoTaskMemFree(id);
  }
  const int width = desktop.right - desktop.left, height = desktop.bottom - desktop.top;
  if (width <= 0 || height <= 0) return {};
  // A single cached surface covers different wallpapers and negative monitor origins.
  const int step = std::max(4, int(std::ceil(std::max(width, height) / 3072.0)));
  auto output = std::make_shared<WallpaperFrame>();
  output->screen = desktop; output->width = (width + step - 1) / step; output->height = (height + step - 1) / step;
  COLORREF background = 0; wallpaper->GetBackgroundColor(&background);
  const DWORD color = 0xff000000 | (DWORD(GetRValue(background)) << 16) | (DWORD(GetGValue(background)) << 8) | GetBValue(background);
  output->pixels.assign(size_t(output->width) * output->height, color);
  DESKTOP_WALLPAPER_POSITION position = DWPOS_FILL; wallpaper->GetPosition(&position);
  std::map<std::wstring, Image> images;
  for (const auto &monitor : monitors) {
    const auto path = position == DWPOS_SPAN || position == DWPOS_TILE ? monitors.front().path : monitor.path;
    auto [entry, inserted] = images.try_emplace(path);
    if (inserted) entry->second = decode(factory.Get(), path);
    const auto &image = entry->second;
    if (image.pixels.empty()) continue;
    const auto area = position == DWPOS_SPAN || position == DWPOS_TILE ? desktop : monitor.bounds;
    const auto placement = placeWallpaper(image.original, area, position);
    for (int y = 0; y < output->height; ++y) for (int x = 0; x < output->width; ++x) {
      const double sx = desktop.left + (x + 0.5) * width / output->width;
      const double sy = desktop.top + (y + 0.5) * height / output->height;
      if (sx < monitor.bounds.left || sx >= monitor.bounds.right || sy < monitor.bounds.top || sy >= monitor.bounds.bottom) continue;
      double u = (sx - placement.left) / placement.width, v = (sy - placement.top) / placement.height;
      if (position == DWPOS_TILE) {
        u = sx / image.original.cx; v = sy / image.original.cy;
        u -= std::floor(u); v -= std::floor(v);
      }
      if (u < 0 || v < 0 || u >= 1 || v >= 1) continue;
      output->pixels[size_t(y) * output->width + x] = 0xff000000 | image.pixels[
          size_t(std::min(image.height - 1, UINT(v * image.height))) * image.width + std::min(image.width - 1, UINT(u * image.width))];
    }
  }
  return output;
}
struct Cache {
  std::shared_ptr<const WallpaperFrame> image;
  std::future<std::shared_ptr<const WallpaperFrame>> pending;
  std::future<std::shared_ptr<const WallpaperFrame>> filtering;
  std::shared_ptr<const WallpaperFrame> filterSource;
  int filterRadius = 0;
  std::map<int, std::weak_ptr<const WallpaperFrame>> variants;
  std::deque<std::shared_ptr<const WallpaperFrame>> recent;
  unsigned generation = 1, requested = 0, completed = 0;
  ULONGLONG retry = 0;
} cache;
}
std::shared_ptr<const WallpaperFrame> wallpaperBackdrop(int strength) {
  if (cache.pending.valid() && cache.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      auto image = cache.pending.get();
      if (image && cache.requested == cache.generation) {
        cache.image = std::move(image); cache.completed = cache.requested;
        cache.variants.clear(); cache.recent.clear();
      }
    } catch (...) {}
    cache.retry = GetTickCount64() + 10000;
  }
  if (!cache.pending.valid() && (cache.requested != cache.generation || (cache.completed != cache.generation && GetTickCount64() >= cache.retry))) {
    std::packaged_task<std::shared_ptr<const WallpaperFrame>()> task(render);
    cache.requested = cache.generation; cache.pending = task.get_future();
    std::thread(std::move(task)).detach();
  }
  if (cache.filtering.valid() && cache.filtering.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try {
      auto image = cache.filtering.get();
      if (image && cache.filterSource == cache.image) {
        cache.variants[cache.filterRadius] = image;
        cache.recent.push_back(std::move(image));
        if (cache.recent.size() > 3) cache.recent.pop_front();
      }
    } catch (...) {}
    cache.filterSource.reset();
  }
  if (!cache.image) return {};
  const int radius = wallpaperBlurRadius(*cache.image, strength);
  if (!radius) return cache.image;
  if (auto image = cache.variants[radius].lock()) return image;
  if (!cache.filtering.valid()) {
    // At most one filter job; changing the slider cannot queue unbounded work.
    auto source = cache.image;
    std::packaged_task<std::shared_ptr<const WallpaperFrame>()> task([source, radius] {
      auto image = std::make_shared<WallpaperFrame>(*source);
      blurWallpaper(*image, radius);
      return image;
    });
    cache.filterSource = std::move(source); cache.filterRadius = radius;
    cache.filtering = task.get_future(); std::thread(std::move(task)).detach();
  }
  return {};
}
void invalidateWallpaperBackdrop() { ++cache.generation; }
}
