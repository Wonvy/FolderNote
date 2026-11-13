#include "context_order.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <shlobj.h>
#include <wrl/client.h>

namespace context {
namespace {
using Microsoft::WRL::ComPtr;

struct FreePidl {
  using pointer = PIDLIST_ABSOLUTE;
  void operator()(PIDLIST_ABSOLUTE value) const noexcept { CoTaskMemFree(value); }
};
struct Popup {
  HMENU handle = CreatePopupMenu();
  ~Popup() { if (handle) DestroyMenu(handle); }
};
struct Bitmap {
  HBITMAP handle = nullptr;
  ~Bitmap() { if (handle) DeleteObject(handle); }
};
struct DrawDc {
  HDC handle = CreateCompatibleDC(nullptr);
  ~DrawDc() { if (handle) DeleteDC(handle); }
};
using Pixels = std::vector<std::uint32_t>;

BITMAPINFO bitmapInfo(int width, int height) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  return info;
}

MenuIcon makeIcon(const Pixels &pixels, int width, int height) {
  const auto info = bitmapInfo(width, height);
  void *data = nullptr;
  Bitmap color{CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0)};
  const int stride = ((width + 15) / 16) * 2;
  std::vector<BYTE> maskBits(static_cast<size_t>(stride) * height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      if (!(pixels[static_cast<size_t>(y) * width + x] >> 24))
        maskBits[static_cast<size_t>(y) * stride + x / 8] |= static_cast<BYTE>(0x80u >> (x % 8));
  Bitmap mask{CreateBitmap(width, height, 1, 1, maskBits.data())};
  if (!color.handle || !mask.handle) return {};
  std::memcpy(data, pixels.data(), pixels.size() * sizeof(pixels.front()));
  ICONINFO icon{TRUE, 0, 0, mask.handle, color.handle};
  auto handle = CreateIconIndirect(&icon);
  return handle ? MenuIcon(handle, DestroyIcon) : MenuIcon{};
}

MenuIcon bitmapIcon(HBITMAP bitmap) {
  BITMAP shape{};
  // GetObject also rejects HBMMENU_* pseudo handles.
  if (!bitmap || GetObjectW(bitmap, sizeof(shape), &shape) != sizeof(shape) ||
      shape.bmWidth <= 0 || shape.bmHeight <= 0 || shape.bmWidth > 256 || shape.bmHeight > 256) return {};
  auto info = bitmapInfo(shape.bmWidth, shape.bmHeight);
  DrawDc dc;
  Pixels pixels(static_cast<size_t>(shape.bmWidth) * shape.bmHeight);
  if (!dc.handle || GetDIBits(dc.handle, bitmap, 0, shape.bmHeight, pixels.data(), &info, DIB_RGB_COLORS) != shape.bmHeight)
    return {};
  const bool hasAlpha = shape.bmBitsPixel == 32 &&
      std::any_of(pixels.begin(), pixels.end(), [](auto pixel) { return (pixel >> 24) != 0; });
  if (!hasAlpha)
    for (auto &pixel : pixels) pixel |= 0xff000000u;
  return makeIcon(pixels, shape.bmWidth, shape.bmHeight);
}

class MenuMessages {
public:
  explicit MenuMessages(IContextMenu *context) : context_(context) {
    if (FAILED(context->QueryInterface(IID_PPV_ARGS(&menu3_))))
      context->QueryInterface(IID_PPV_ARGS(&menu2_));
  }
  bool send(UINT message, WPARAM parameter, LPARAM data) const {
    LRESULT result = 0;
    if (menu3_) return menu3_->HandleMenuMsg2(message, parameter, data, &result) == S_OK;
    return menu2_ && menu2_->HandleMenuMsg(message, parameter, data) == S_OK;
  }
  std::wstring canonicalVerb(UINT id) const {
    if (id < 1 || id > 0x7fff) return {};
    std::array<wchar_t, 256> verb{};
    if (context_->GetCommandString(id - 1, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb.data()),
                                   static_cast<UINT>(verb.size())) != S_OK) return {};
    return {verb.begin(), std::find(verb.begin(), verb.end(), L'\0')};
  }
private:
  IContextMenu *context_;
  ComPtr<IContextMenu3> menu3_;
  ComPtr<IContextMenu2> menu2_;
};

Pixels drawCallbackBitmap(const MenuMessages &messages, HMENU menu, const MENUITEMINFOW &item,
                          int width, int height, std::uint32_t background) {
  DrawDc dc;
  void *data = nullptr;
  const auto info = bitmapInfo(width, height);
  Bitmap bitmap{CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0)};
  if (!dc.handle || !bitmap.handle) return {};
  const size_t size = static_cast<size_t>(width) * height;
  auto pixels = static_cast<std::uint32_t *>(data);
  std::fill_n(pixels, size, background);
  auto previous = SelectObject(dc.handle, bitmap.handle);
  if (!previous || previous == HGDI_ERROR) return {};
  IntersectClipRect(dc.handle, 0, 0, width, height);
  DRAWITEMSTRUCT draw{ODT_MENU, 0, item.wID, ODA_DRAWENTIRE, 0, reinterpret_cast<HWND>(menu),
                      dc.handle, {0, 0, width, height}, item.dwItemData};
  const bool painted = messages.send(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
  GdiFlush();
  SelectObject(dc.handle, previous);
  return painted ? Pixels(pixels, pixels + size) : Pixels{};
}

MenuIcon callbackIcon(const MenuMessages &messages, HMENU menu, const MENUITEMINFOW &item) {
  MEASUREITEMSTRUCT size{ODT_MENU, 0, item.wID, 0, 0, item.dwItemData};
  if (!messages.send(WM_MEASUREITEM, 0, reinterpret_cast<LPARAM>(&size)) ||
      !size.itemWidth || !size.itemHeight || size.itemWidth > 64 || size.itemHeight > 64) return {};
  const int width = static_cast<int>(size.itemWidth), height = static_cast<int>(size.itemHeight);
  auto black = drawCallbackBitmap(messages, menu, item, width, height, 0xff000000u);
  auto white = drawCallbackBitmap(messages, menu, item, width, height, 0xffffffffu);
  if (black.empty() || white.empty()) return {};
  // A transparent pixel passes the backdrop through unchanged. Two backdrops
  // let us retain anti-aliased edges even when the handler draws with GDI.
  bool hasInk = false;
  for (size_t i = 0; i < black.size(); ++i) {
    int difference = 0;
    for (int shift = 0; shift < 24; shift += 8)
      difference += static_cast<int>((white[i] >> shift) & 0xffu) - static_cast<int>((black[i] >> shift) & 0xffu);
    const auto alpha = static_cast<unsigned>(255 - std::clamp((difference + 1) / 3, 0, 255));
    black[i] = (black[i] & 0x00ffffffu) | (alpha << 24);
    hasInk |= alpha != 0;
  }
  return hasInk ? makeIcon(black, width, height) : MenuIcon{};
}

ComPtr<IContextMenu> contextMenu(HWND owner, const std::wstring &path, PreviewTarget target) {
  ComPtr<IShellFolder> folder;
  ComPtr<IContextMenu> menu;
  if (target == PreviewTarget::Background && path.empty()) {
    if (SUCCEEDED(SHGetDesktopFolder(&folder)))
      folder->CreateViewObject(owner, IID_PPV_ARGS(&menu));
    return menu;
  }
  if (path.empty()) return {};

  PIDLIST_ABSOLUTE parsed = nullptr;
  const auto parseResult = SHParseDisplayName(path.c_str(), nullptr, &parsed, 0, nullptr);
  std::unique_ptr<ITEMIDLIST, FreePidl> selection(parsed);
  if (FAILED(parseResult) || !selection) return {};
  if (target == PreviewTarget::Background) {
    ComPtr<IShellFolder> desktop;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop)) &&
        SUCCEEDED(desktop->BindToObject(parsed, nullptr, IID_PPV_ARGS(&folder))))
      folder->CreateViewObject(owner, IID_PPV_ARGS(&menu));
  } else {
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(parsed, IID_PPV_ARGS(&folder), &child)) && child)
      folder->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void **>(menu.GetAddressOf()));
  }
  return menu;
}
struct MenuIdentity {
  bool hasSubmenu = false;
  std::wstring canonicalVerb;
};
std::vector<MenuPreviewItem> readMenu(HMENU menu, const MenuMessages &messages, bool readChildren,
                                      std::vector<MenuIdentity> *identities = nullptr, bool readIcons = true) {
  std::vector<MenuPreviewItem> entries;
  const int count = GetMenuItemCount(menu);
  for (int index = 0; index < count; ++index) {
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_BITMAP | MIIM_DATA | MIIM_ID | MIIM_CHECKMARKS | MIIM_STATE;
    if (readChildren || identities) item.fMask |= MIIM_SUBMENU;
    if (!GetMenuItemInfoW(menu, index, TRUE, &item)) continue;
    if (item.fType & MFT_SEPARATOR) {
      entries.push_back({{}, {}, {}, true, {}});
      if (identities) identities->push_back({});
      continue;
    }
    if (!item.cch || item.cch > 4096) continue;
    std::wstring caption(static_cast<size_t>(item.cch) + 1, L'\0');
    item.dwTypeData = caption.data();
    item.cch = static_cast<UINT>(caption.size());
    if (GetMenuItemInfoW(menu, index, TRUE, &item) && item.cch) {
      caption.resize(item.cch);
      std::wstring display;
      for (size_t i = 0; i < caption.size() && caption[i] != L'\t'; ++i) {
        if (caption[i] == L'&') {
          if (i + 1 >= caption.size() || caption[i + 1] != L'&') continue;
          ++i;
        }
        display += caption[i];
      }
      if (display.empty()) continue;
      MenuIcon icon;
      if (readIcons) {
        icon = item.hbmpItem == HBMMENU_CALLBACK && !(item.fType & MFT_OWNERDRAW)
            ? callbackIcon(messages, menu, item) : bitmapIcon(item.hbmpItem);
        // Older extensions put a custom icon in the menu's checkmark gutter.
        if (!icon) icon = bitmapIcon((item.fState & MFS_CHECKED) ? item.hbmpChecked : item.hbmpUnchecked);
      }
      std::vector<MenuPreviewItem> children;
      if (readChildren && item.hSubMenu) {
        const auto submenu = reinterpret_cast<WPARAM>(item.hSubMenu);
        messages.send(WM_INITMENUPOPUP, submenu, MAKELPARAM(index, FALSE));
        children = readMenu(item.hSubMenu, messages, false);
        messages.send(WM_UNINITMENUPOPUP, submenu, 0);
      }
      entries.push_back({std::move(display), std::move(icon), std::move(children), false, {}});
      if (identities) identities->push_back({item.hSubMenu != nullptr, messages.canonicalVerb(item.wID)});
    }
  }
  return entries;
}

void resolveHandlers(HWND owner, const std::wstring &path, PreviewTarget target,
                     std::span<const std::wstring> candidates, std::span<const std::wstring> hiddenCandidates,
                     std::span<const MenuIdentity> identities, std::vector<MenuPreviewItem> &entries,
                     std::vector<MenuPreviewItem> *hiddenItems) {
  using Pidl = std::unique_ptr<ITEMIDLIST, FreePidl>;
  PIDLIST_ABSOLUTE raw = nullptr;
  const auto result = target == PreviewTarget::Background && path.empty()
      ? SHGetSpecialFolderLocation(owner, CSIDL_DESKTOP, &raw)
      : SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr);
  Pidl location(raw);
  if (FAILED(result) || !location) return;

  ComPtr<IDataObject> selection;
  if (target == PreviewTarget::Object) {
    ComPtr<IShellFolder> folder;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(location.get(), IID_PPV_ARGS(&folder), &child)) || !child ||
        FAILED(folder->GetUIObjectOf(owner, 1, &child, IID_IDataObject, nullptr,
                                     reinterpret_cast<void **>(selection.GetAddressOf()))))
      return;
    if (!ILRemoveLastID(location.get())) return;
  }

  std::vector<bool> ambiguous(entries.size());
  auto query = [&](const std::wstring &candidate, bool hidden, std::vector<MenuIdentity> *observedIdentities) {
    CLSID id{};
    if (candidate.size() != 38 || candidate.front() != L'{' || candidate.back() != L'}' ||
        FAILED(CLSIDFromString(candidate.c_str(), &id))) return std::vector<MenuPreviewItem>{};
    ComPtr<IShellExtInit> init;
    if (FAILED(CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&init))) ||
        FAILED(init->Initialize(location.get(), selection.Get(), nullptr))) return std::vector<MenuPreviewItem>{};
    ComPtr<IContextMenu> context;
    if (FAILED(init.As(&context))) return std::vector<MenuPreviewItem>{};
    Popup popup;
    if (!popup.handle || FAILED(context->QueryContextMenu(popup.handle, 0, 1, 0x7fff, CMF_NORMAL)))
      return std::vector<MenuPreviewItem>{};
    return readMenu(popup.handle, MenuMessages(context.Get()), hidden, observedIdentities, hidden);
  };
  for (const auto &candidate : candidates) {
    std::vector<MenuIdentity> observedIdentities;
    const auto items = query(candidate, false, &observedIdentities);
    for (size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
      const auto &item = items[itemIndex];
      if (item.separator) continue;
      const auto &observedIdentity = observedIdentities[itemIndex];
      size_t unique = entries.size();
      for (size_t i = 0; i < entries.size(); ++i) {
        const auto &identity = identities[i];
        if (entries[i].caption != item.caption || identity.hasSubmenu != observedIdentity.hasSubmenu) continue;
        if (!identity.canonicalVerb.empty() && !observedIdentity.canonicalVerb.empty() &&
            CompareStringOrdinal(identity.canonicalVerb.c_str(), -1, observedIdentity.canonicalVerb.c_str(), -1,
                                 TRUE) != CSTR_EQUAL) continue;
        if (unique != entries.size()) { unique = entries.size(); break; }
        unique = i;
      }
      if (unique == entries.size() || ambiguous[unique]) continue;
      auto &observed = entries[unique].handlerClsid;
      if (observed.empty() || observed == candidate) observed = candidate;
      else { observed.clear(); ambiguous[unique] = true; }
    }
  }
  if (hiddenItems) {
    for (const auto &candidate : hiddenCandidates)
      for (auto &item : query(candidate, true, nullptr)) {
        if (item.separator) continue;
        item.handlerClsid = candidate;
        hiddenItems->push_back(std::move(item));
      }
  }
}
}

std::vector<MenuPreviewItem> actualMenuOrder(HWND owner, const std::wstring &path, PreviewTarget target,
                                             std::span<const std::wstring> handlerCandidates,
                                             std::span<const std::wstring> hiddenHandlerCandidates,
                                             std::vector<MenuPreviewItem> *hiddenHandlerItems) {
  if (hiddenHandlerItems) hiddenHandlerItems->clear();
  auto context = contextMenu(owner, path, target);
  Popup popup;
  if (!context || !popup.handle ||
      FAILED(context->QueryContextMenu(popup.handle, 0, 1, 0x7fff, CMF_NORMAL))) return {};
  std::vector<MenuIdentity> identities;
  auto entries = readMenu(popup.handle, MenuMessages(context.Get()), true,
                          handlerCandidates.empty() ? nullptr : &identities);
  if ((!entries.empty() && !handlerCandidates.empty()) || (hiddenHandlerItems && !hiddenHandlerCandidates.empty()))
    resolveHandlers(owner, path, target, handlerCandidates, hiddenHandlerCandidates, identities, entries, hiddenHandlerItems);
  return entries;
}
}
