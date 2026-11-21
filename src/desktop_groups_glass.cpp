#include "desktop_groups_glass.h"
#include "desktop_groups_zorder.h"
#include <algorithm>
#include <utility>

namespace desktop_groups {
DesktopGlass::~DesktopGlass() { clear(); }
void DesktopGlass::clear() {
  if (window) DestroyWindow(window);
  window = nullptr; frame.reset(); shown = false;
}
LRESULT CALLBACK DesktopGlass::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  auto self = reinterpret_cast<DesktopGlass *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<DesktopGlass *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    self->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_NCDESTROY && self) { self->window = nullptr; self->shown = false; self->frame.reset(); }
  return DefWindowProcW(window, message, w, l);
}
bool DesktopGlass::update(HWND host, std::shared_ptr<const WallpaperFrame> image, std::span<const GlassArea> areas, int strength) {
  if (window && GetParent(window) != host) clear();
  if (!image && frame) image = frame; // Keep the previous frame until asynchronous filtering finishes.
  if (!IsWindow(host) || !image || areas.empty()) {
    if (window) ShowWindow(window, SW_HIDE);
    shown = false; return true;
  }
  DesktopDpiScope dpi(host);
  if (!window) {
    WNDCLASSW cls{}; cls.lpfnWndProc = proc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"FolderNote.DesktopGroups.Glass";
    RegisterClassW(&cls);
    if (!CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        // Keep pixels under the transparent WinUI siblings: WS_CLIPSIBLINGS
        // would clip away exactly the box regions that need this backdrop.
        cls.lpszClassName, L"", WS_CHILD, 0, 0, 0, 0, host, nullptr, cls.hInstance, this)) return false;
  }
  if (image != frame) {
    const int width = image->screen.right - image->screen.left, height = image->screen.bottom - image->screen.top;
    if (width <= 0 || height <= 0 || size_t(width) * height > 64 * 1024 * 1024) return false;
    BITMAPINFO destination{}; destination.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void *pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &destination, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto dc = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    const auto old = dc ? SelectObject(dc, bitmap) : nullptr;
    bool rendered = false;
    if (old && old != HGDI_ERROR) {
      BITMAPINFO sourceInfo{}; sourceInfo.bmiHeader = {sizeof(BITMAPINFOHEADER), image->width, -image->height, 1, 32, BI_RGB};
      SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
      const auto copied = StretchDIBits(dc, 0, 0, width, height, 0, 0, image->width, image->height,
          image->pixels.data(), &sourceInfo, DIB_RGB_COLORS, SRCCOPY);
      POINT position = desktopWindowPoint(window, {image->screen.left, image->screen.top}), source{};
      SIZE size{width, height};
      rendered = copied && copied != GDI_ERROR && UpdateLayeredWindow(window, nullptr, &position, &size,
          dc, &source, 0, nullptr, ULW_OPAQUE);
      SelectObject(dc, old);
    }
    if (dc) DeleteDC(dc);
    if (bitmap) DeleteObject(bitmap);
    if (!rendered) return false;
    frame = std::move(image);
  }
  // The wallpaper bitmap never moves with a box. Only its desktop-space mask
  // changes, avoiding a separately committed XAML image offset on every frame.
  const auto region = CreateRectRgn(0, 0, 0, 0), current = CreateRectRgn(0, 0, 0, 0), occupied = CreateRectRgn(0, 0, 0, 0);
  if (!region || !current || !occupied) {
    if (region) DeleteObject(region); if (current) DeleteObject(current); if (occupied) DeleteObject(occupied); return false;
  }
  for (const auto &area : areas) {
    const int width = area.bounds.right - area.bounds.left, height = area.bounds.bottom - area.bounds.top;
    const auto diameter = std::min({area.diameter, width, height});
    const auto part = diameter > 0 ? CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter)
                                  : CreateRectRgn(0, 0, width, height);
    if (!part) { DeleteObject(region); DeleteObject(current); DeleteObject(occupied); return false; }
    OffsetRgn(part, area.bounds.left - frame->screen.left, area.bounds.top - frame->screen.top);
    if (area.strength == strength) {
      CombineRgn(current, part, occupied, RGN_DIFF);
      CombineRgn(region, region, current, RGN_OR);
    }
    CombineRgn(occupied, occupied, part, RGN_OR); DeleteObject(part);
  }
  DeleteObject(occupied);
  const bool same = GetWindowRgn(window, current) != ERROR && EqualRgn(region, current);
  DeleteObject(current);
  if (same) DeleteObject(region);
  else if (!SetWindowRgn(window, region, FALSE)) { DeleteObject(region); return false; }
  shown = true;
  return true;
}
}
