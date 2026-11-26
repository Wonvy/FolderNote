#include "desktop_groups_icon_source.h"
#include "desktop_groups_model.h"
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <memory>
#include <utility>

namespace desktop_groups {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto recycleBin = L"::{645FF040-5081-101B-9F08-00AA002F954E}";

std::wstring displayPath(IShellItem *item, SIGDN form) {
  PWSTR raw = nullptr;
  const auto result = item->GetDisplayName(form, &raw);
  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
  if (FAILED(result) || !path) return {};
  return path.get();
}

}

HBITMAP loadDesktopThumbnail(const std::wstring &path, int pixels) {
  if (pixels <= 0) return nullptr;
  ComPtr<IShellItemImageFactory> factory;
  if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory) return nullptr;
  using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)>;
  HBITMAP image = nullptr;
  auto result = factory->GetImage({pixels, pixels}, SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT | SIIGBF_SCALEUP, &image);
  Bitmap source(image, DeleteObject);
  if (FAILED(result)) {
    source.reset();
    image = nullptr;
    result = factory->GetImage({pixels, pixels}, SIIGBF_ICONONLY | SIIGBF_RESIZETOFIT | SIIGBF_SCALEUP, &image);
    source.reset(image);
  }
  BITMAP dimensions{};
  if (FAILED(result) || !source || !GetObjectW(source.get(), sizeof(dimensions), &dimensions) ||
      dimensions.bmWidth <= 0 || dimensions.bmHeight <= 0) return nullptr;
  if (dimensions.bmWidth == pixels && dimensions.bmHeight == pixels) return source.release();

  // Image lists require square slots, but the photograph itself must keep its
  // aspect ratio. Pad with transparency instead of cropping or stretching it.
  const auto longest = std::max(dimensions.bmWidth, dimensions.bmHeight);
  const int width = std::clamp(MulDiv(dimensions.bmWidth, pixels, longest), 1, pixels);
  const int height = std::clamp(MulDiv(dimensions.bmHeight, pixels, longest), 1, pixels);
  BITMAPINFO format{};
  format.bmiHeader = {sizeof(BITMAPINFOHEADER), pixels, -pixels, 1, 32, BI_RGB};
  void *data = nullptr;
  Bitmap canvas(CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &data, nullptr, 0), DeleteObject);
  if (!canvas || !data) return nullptr;
  std::fill_n(static_cast<DWORD *>(data), static_cast<size_t>(pixels) * pixels, 0);
  using DC = std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)>;
  const DC from(CreateCompatibleDC(nullptr), DeleteDC), to(CreateCompatibleDC(nullptr), DeleteDC);
  if (!from || !to) return nullptr;
  const auto oldSource = SelectObject(from.get(), source.get());
  const auto oldTarget = SelectObject(to.get(), canvas.get());
  const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  const bool copied = AlphaBlend(to.get(), (pixels - width) / 2, (pixels - height) / 2, width, height,
                                from.get(), 0, 0, dimensions.bmWidth, dimensions.bmHeight, blend) != FALSE;
  SelectObject(from.get(), oldSource);
  SelectObject(to.get(), oldTarget);
  return copied ? canvas.release() : nullptr;
}

int setDesktopThumbnail(HIMAGELIST images, int index, HBITMAP bitmap) {
  BITMAP dimensions{};
  int width = 0, height = 0;
  if (!images || !bitmap || !ImageList_GetIconSize(images, &width, &height) || width <= 0 || height <= 0 ||
      !GetObjectW(bitmap, sizeof(dimensions), &dimensions) || dimensions.bmWidth != width || dimensions.bmHeight != height)
    return -1;
  // A null mask on replacement retains the placeholder icon's old mask. In
  // particular a padded jumbo icon can clip a full-size thumbnail to a tiny
  // rectangle. Reset the mask; the 32-bit bitmap supplies its own alpha.
  const auto stride = ((static_cast<size_t>(width) + 15) / 16) * 2;
  std::vector<BYTE> opaque(stride * height, 0);
  const std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> mask(
      CreateBitmap(width, height, 1, 1, opaque.data()), DeleteObject);
  if (!mask) return -1;
  return index < 0 ? ImageList_Add(images, bitmap, mask.get()) :
      ImageList_Replace(images, index, bitmap, mask.get()) ? index : -1;
}

std::vector<std::wstring> desktopShellDropItems(IDataObject *data) {
  ComPtr<IShellItemArray> items;
  DWORD count = 0;
  if (!data || FAILED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items))) ||
      !items || FAILED(items->GetCount(&count))) return {};
  std::vector<std::wstring> paths;
  paths.reserve(count);
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
    if (auto path = displayPath(item.Get(), SIGDN_FILESYSPATH); !path.empty()) {
      paths.push_back(std::move(path));
    } else if (samePath(displayPath(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING), recycleBin)) {
      paths.emplace_back(recycleBin);
    }
  }
  return paths;
}
}
