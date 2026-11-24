#include "../src/desktop_groups_wallpaper.h"
#include "../src/desktop_groups_glass.h"
#include "../src/desktop_groups_zorder.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace { void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); } }
int main(int argc, char **argv) {
  using namespace desktop_groups;
  try {
    const RECT monitor{-1920, 100, 0, 1180};
    auto fit = placeWallpaper({1000, 1000}, monitor, DWPOS_FIT);
    check(fit.left == -1500 && fit.top == 100 && fit.width == 1080 && fit.height == 1080, "fit preserves aspect ratio and letterbox");
    auto fill = placeWallpaper({1000, 1000}, monitor, DWPOS_FILL);
    check(fill.left == -1920 && fill.top == -320 && fill.width == 1920 && fill.height == 1920, "fill uses correct crop at negative monitor origin");
    auto center = placeWallpaper({1000, 500}, monitor, DWPOS_CENTER);
    check(center.left == -1460 && center.top == 390 && center.width == 1000, "center does not rescale");
    auto stretch = placeWallpaper({1000, 500}, monitor, DWPOS_STRETCH);
    check(stretch.width == 1920 && stretch.height == 1080, "stretch covers exact monitor dimensions");
    WallpaperFrame frame{monitor, 120, 80, std::vector<DWORD>(120 * 80, 0xff204060)};
    blurWallpaper(frame, 4);
    check(std::all_of(frame.pixels.begin(), frame.pixels.end(), [](DWORD c) { return c == 0xff204060; }), "blur preserves constant opaque colors");
    for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x)
      frame.pixels[size_t(y) * frame.width + x] = x < 60 ? 0xff000000 : 0xffffffff;
    const auto sharp = frame;
    blurWallpaper(frame, 0);
    check(frame.pixels == sharp.pixels, "zero blur preserves sharp pixels");
    auto stronger = sharp;
    blurWallpaper(stronger, 12);
    blurWallpaper(frame, 4);
    const auto darkEdge = frame.pixels[40 * 120 + 59] & 255, lightEdge = frame.pixels[40 * 120 + 60] & 255;
    check(darkEdge > 80 && lightEdge < 180 && darkEdge < lightEdge, "blur softens sharp wallpaper boundaries");
    check((frame.pixels.front() & 255) == 0 && (frame.pixels.back() & 255) == 255, "blur does not wrap opposite edges");
    check(std::all_of(frame.pixels.begin(), frame.pixels.end(), [](DWORD c) { return (c >> 24) == 255; }), "blur keeps opaque alpha");
    check((stronger.pixels[40 * 120 + 48] & 255) > (frame.pixels[40 * 120 + 48] & 255), "stronger blur spreads the edge farther");
    WallpaperFrame quarter{{0,0,480,320},120,80,{}};
    check(wallpaperBlurRadius(quarter, 0) == 0 && wallpaperBlurRadius(quarter, defaultBlurStrength) == 4 &&
          wallpaperBlurRadius(quarter, 100) == 16, "slider covers zero through stronger blur and retains the old default");
    check(isSolidColorWallpaper(L"", 0) && isSolidColorWallpaper(L"   ", 0) &&
          isSolidColorWallpaper(L"C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg", 1) &&
          !isSolidColorWallpaper(L"C:\\Windows\\Web\\Wallpaper\\Windows\\img0.jpg", 0) &&
          !isSolidColorWallpaper(L"C:\\Users\\Test\\AppData\\Roaming\\Microsoft\\Windows\\Themes\\TranscodedWallpaper", 2),
          "solid color desktop mode is detected when SPI wallpaper path is empty or BackgroundType is 1");
    {
      const auto host = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
          -32000, -32000, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      check(host != nullptr, "create isolated glass host");
      auto image = std::make_shared<WallpaperFrame>(WallpaperFrame{{-32000,-32000,-31200,-31400}, 80, 60, std::vector<DWORD>(80*60,0xff204060)});
      DesktopGlass glass;
      for (int i = 0; i < 50; ++i) {
        const GlassArea areas[]{{{-31980+i*4,-31980,-31780+i*4,-31780},12}};
        check(glass.update(host, image, areas) && glass.handle(), "update glass mask during drag");
        RECT actual{}; GetWindowRect(glass.handle(), &actual);
        check(EqualRect(&actual, &image->screen), "drag never changes wallpaper origin or dimensions");
        const auto region = CreateRectRgn(0,0,0,0);
        check(GetWindowRgn(glass.handle(),region) != ERROR && PtInRegion(region,120+i*4,120) && !PtInRegion(region,10,10),
              "glass clips to the moving box without covering native desktop elsewhere");
        DeleteObject(region);
        check(GetParent(glass.handle()) == host && !(GetWindowLongPtrW(glass.handle(),GWL_EXSTYLE)&WS_EX_TOPMOST), "glass stays inside desktop layer");
      }
      check(glass.update(host,image,{}) && !glass.handle(), "turning glass off removes its visible layer");
      DesktopGlass frontGlass;
      GlassArea overlap[]{
        {{-31900,-31900,-31700,-31700},12,80},
        {{-31950,-31950,-31650,-31650},12,25}
      };
      check(frontGlass.update(host,image,overlap,80) && glass.update(host,image,overlap,25), "different blur layers can overlap");
      const auto front = CreateRectRgn(0,0,0,0), back = CreateRectRgn(0,0,0,0);
      check(GetWindowRgn(frontGlass.handle(),front) != ERROR && GetWindowRgn(glass.handle(),back) != ERROR &&
            PtInRegion(front,150,150) && !PtInRegion(back,150,150) && PtInRegion(back,75,75),
            "front box masks out the other box's blur");
      overlap[0].strength = -1;
      check(glass.update(host,image,overlap,25) && GetWindowRgn(glass.handle(),back) != ERROR && !PtInRegion(back,150,150),
            "a box with blur disabled also occludes another box's blur");
      std::swap(overlap[0],overlap[1]);
      check(glass.update(host,image,overlap,25) && GetWindowRgn(glass.handle(),back) != ERROR && PtInRegion(back,150,150),
            "raising the back box updates the overlapping blur mask immediately");
      check(glass.update(host,{},overlap,25) && glass.handle(), "keep the existing wallpaper while the next blur is pending");
      DeleteObject(front); DeleteObject(back);
      DestroyWindow(host);
      check(!glass.handle() && !frontGlass.handle(), "parent destruction clears backdrop handles");
    }
    if (argc == 2) {
      std::shared_ptr<const WallpaperFrame> desktop;
      for (int i = 0; i < 200 && !desktop; ++i) { desktop = wallpaperBackdrop(); Sleep(25); }
      check(desktop && !desktop->pixels.empty(), "decode actual wallpaper asynchronously");
      BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
      file.bfSize = file.bfOffBits + DWORD(desktop->pixels.size() * 4);
      BITMAPINFOHEADER info{sizeof(info), desktop->width, -desktop->height, 1, 32, BI_RGB};
      std::ofstream out(argv[1], std::ios::binary);
      out.write(reinterpret_cast<const char *>(&file), sizeof(file)); out.write(reinterpret_cast<const char *>(&info), sizeof(info));
      out.write(reinterpret_cast<const char *>(desktop->pixels.data()), desktop->pixels.size() * 4);
      std::cout << "Wallpaper snapshot: " << desktop->width << 'x' << desktop->height << '\n';
      check(wallpaperBackdrop() == desktop, "movement reuses cached bitmap");
      std::shared_ptr<const WallpaperFrame> maximum;
      for (int i = 0; i < 200 && !maximum; ++i) { maximum = wallpaperBackdrop(100); Sleep(25); }
      auto unfiltered = wallpaperBackdrop(0);
      check(maximum && unfiltered && maximum != desktop && unfiltered != desktop && maximum != unfiltered,
            "zero, default and maximum strengths use separate cached images");
      check(wallpaperBackdrop() == desktop && wallpaperBackdrop(100) == maximum,
            "different boxes reuse their own cached blur strengths");
    }
    std::cout << "PASS wallpaper placement, blur, edge sampling and alpha\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
