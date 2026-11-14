#include "desktop_groups_render.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;

D2D1_COLOR_F color(COLORREF value) {
  return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f);
}

D2D1_RECT_F local(const RECT &area, const RECT &origin) {
  return D2D1::RectF(static_cast<float>(area.left - origin.left), static_cast<float>(area.top - origin.top),
                    static_cast<float>(area.right - origin.left), static_cast<float>(area.bottom - origin.top));
}

struct Renderer {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> write;
  ComPtr<ID2D1DCRenderTarget> target;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<IDWriteTextFormat> rowText, iconText;
  HDC buffer = CreateCompatibleDC(nullptr);
  HBITMAP image = nullptr;
  HGDIOBJ original = nullptr;
  int width = 0, height = 0;
  UINT fontDpi = 0;

  ~Renderer() {
    brush.Reset();
    target.Reset();
    if (buffer && original) SelectObject(buffer, original);
    if (image) DeleteObject(image);
    if (buffer) DeleteDC(buffer);
  }

  bool prepare(int neededWidth, int neededHeight, UINT dpi) {
    if (!buffer) return false;
    if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
    if (!write && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                       reinterpret_cast<IUnknown **>(write.GetAddressOf())))) return false;
    if (!target) {
      const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
      if (FAILED(d2d->CreateDCRenderTarget(&props, &target))) return false;
      target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!brush && FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &brush))) return false;
    if (fontDpi != dpi) {
      rowText.Reset();
      iconText.Reset();
      if (!format(dpi, true, rowText) || !format(dpi, false, iconText)) return false;
      fontDpi = dpi;
    }
    if (neededWidth <= width && neededHeight <= height) return true;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = std::max(width, neededWidth);
    info.bmiHeader.biHeight = -std::max(height, neededHeight);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *data = nullptr;
    const auto next = CreateDIBSection(buffer, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    if (!next || !data) {
      if (next) DeleteObject(next);
      return false;
    }
    const auto previous = SelectObject(buffer, next);
    if (!previous || previous == HGDI_ERROR) {
      DeleteObject(next);
      return false;
    }
    if (image) DeleteObject(image);
    else original = previous;
    image = next;
    width = info.bmiHeader.biWidth;
    height = -info.bmiHeader.biHeight;
    return true;
  }

  bool format(UINT dpi, bool row, ComPtr<IDWriteTextFormat> &result) {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    GetUserDefaultLocaleName(locale, ARRAYSIZE(locale));
    if (FAILED(write->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, 14.0f * dpi / 96.0f, locale, &result))) return false;
    result->SetTextAlignment(row ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
    result->SetParagraphAlignment(row ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    result->SetWordWrapping(row ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(write->CreateEllipsisTrimmingSign(result.Get(), &ellipsis))) {
      const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      result->SetTrimming(&trimming, ellipsis.Get());
    }
    return true;
  }

};
}

COLORREF boxOutlineColor(COLORREF background, const ui::Palette &colors) {
  if (colors.highContrast) return colors.border;
  const auto mix = [](BYTE base, BYTE ink) { return (base * (255 - boxOutlineAlpha) + ink * boxOutlineAlpha + 127) / 255; };
  return RGB(mix(GetRValue(background), GetRValue(colors.text)),
             mix(GetGValue(background), GetGValue(colors.text)),
             mix(GetBValue(background), GetBValue(colors.text)));
}

HRGN createBoxRegion(int width, int height, int diameter) {
  diameter = std::min({diameter, width, height});
  return diameter > 0 ? CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter)
                      : CreateRectRgn(0, 0, width, height);
}

bool drawBoxShadow(DWORD *pixels, int width, int height, int spread, int offset, int diameter, BYTE opacity) {
  const int boxWidth = width - 2 * spread, boxHeight = height - 2 * spread;
  if (!pixels || spread <= 0 || boxWidth <= 0 || boxHeight <= 0) return false;
  using Region = std::unique_ptr<std::remove_pointer_t<HRGN>, decltype(&DeleteObject)>;
  const Region exterior(CreateRectRgn(0, 0, width, height), DeleteObject);
  const Region box(createBoxRegion(boxWidth, boxHeight, diameter), DeleteObject);
  // The bitmap is offset with the shadow. The hole must stay aligned with the box.
  if (!exterior || !box || OffsetRgn(box.get(), spread, spread - offset) == ERROR ||
      CombineRgn(exterior.get(), exterior.get(), box.get(), RGN_DIFF) == ERROR) return false;
  const DWORD bytes = GetRegionData(exterior.get(), 0, nullptr);
  if (!bytes) return false;
  std::vector<DWORD> storage((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
  auto *data = reinterpret_cast<RGNDATA *>(storage.data());
  if (!GetRegionData(exterior.get(), bytes, data)) return false;
  const auto *bands = reinterpret_cast<const RECT *>(data->Buffer);
  const float radius = std::min({diameter, boxWidth, boxHeight}) / 2.0f;
  const float horizontal = boxWidth / 2.0f - radius;
  const float vertical = boxHeight / 2.0f - radius;
  std::fill_n(pixels, static_cast<size_t>(width) * height, 0);
  for (DWORD i = 0; i < data->rdh.nCount; ++i) {
    const auto &band = bands[i];
    for (LONG y = band.top; y < band.bottom; ++y) {
      const float dy = std::abs(y + 0.5f - height / 2.0f) - vertical;
      auto *row = pixels + static_cast<size_t>(y) * width;
      for (LONG x = band.left; x < band.right; ++x) {
        const float dx = std::abs(x + 0.5f - width / 2.0f) - horizontal;
        const auto distance = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f)) +
                              std::min(std::max(dx, dy), 0.0f) - radius;
        const float strength = 1.0f - std::clamp(distance / spread, 0.0f, 1.0f);
        row[x] = static_cast<DWORD>(opacity * strength * strength) << 24;
      }
    }
  }
  return true;
}

bool drawFenceItem(HDC dc, RECT row, RECT label, RECT icon, HICON image, const std::wstring &text,
                   const ui::Palette &colors, bool selected, bool hovered, bool listMode, UINT dpi) {
  RECT clip{};
  const auto clipType = GetClipBox(dc, &clip);
  if (clipType == ERROR) return false;
  RECT dirty{};
  if (!IntersectRect(&dirty, &row, &clip)) return true;
  const int width = dirty.right - dirty.left, height = dirty.bottom - dirty.top;
  thread_local Renderer render;
  if (!render.prepare(width, height, dpi)) return false;
  const RECT pixels{0, 0, width, height};
  if (FAILED(render.target->BindDC(render.buffer, &pixels))) return false;

  render.target->BeginDraw();
  // Only the scratch surface is cleared. AlphaBlend preserves the caller's
  // wallpaper/background and its original GDI clip when composing the item.
  render.target->Clear(D2D1::ColorF(0, 0, 0, 0));
  if (selected || hovered) {
    auto bounds = local(row, dirty);
    const auto inset = std::max(1.0f, dpi / 96.0f);
    bounds.left += inset; bounds.top += inset; bounds.right -= inset; bounds.bottom -= inset;
    auto fill = color(selected ? (colors.highContrast ? colors.accent : colors.text) : colors.hover);
    fill.a = selected ? (colors.highContrast ? 1.0f : 0.22f) : 0.65f;
    render.brush->SetColor(fill);
    render.target->FillRoundedRectangle(D2D1::RoundedRect(bounds, 5.0f * inset, 5.0f * inset), render.brush.Get());
  }
  if (!text.empty() && !IsRectEmpty(&label)) {
    render.brush->SetColor(color(selected && colors.highContrast ? colors.onAccent : colors.text));
    render.target->DrawTextW(text.data(), static_cast<UINT32>(text.size()),
                             listMode ? render.rowText.Get() : render.iconText.Get(), local(label, dirty),
                             render.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  if (FAILED(render.target->EndDraw())) {
    render.brush.Reset();
    render.target.Reset();
    return false;
  }
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  if (!AlphaBlend(dc, dirty.left, dirty.top, width, height, render.buffer, 0, 0, width, height, blend)) return false;
  if (image && !IsRectEmpty(&icon)) {
    const int size = std::min(icon.right - icon.left, icon.bottom - icon.top);
    const int left = (icon.left + icon.right - size) / 2;
    const int top = listMode ? (icon.top + icon.bottom - size) / 2 : icon.top;
    DrawIconEx(dc, left, top, image, size, size, 0, nullptr, DI_NORMAL);
  }
  return true;
}
}
