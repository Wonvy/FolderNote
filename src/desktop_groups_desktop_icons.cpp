#include "desktop_groups_desktop_icons.h"
#include "quick_folders.h"
#include "theme.h"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <commoncontrols.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shellapi.h>
#include <shlobj.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <windowsx.h>
#include <wrl/client.h>

namespace desktop_groups {
namespace {
constexpr wchar_t hostClass[] = L"FolderNote.DesktopIcons";
constexpr wchar_t marqueeClass[] = L"FolderNote.DesktopIconSelection";
constexpr UINT_PTR dragTimer = 1, thumbnailTimer = 2, selectionTimer = 3;
constexpr UINT selectionStart = WM_APP + 1, selectionToggle = WM_APP + 2;
constexpr UINT selectionFinish = WM_APP + 3, selectionCancel = WM_APP + 4, desktopWheel = WM_APP + 5;

struct Bitmap {
  HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;
  DWORD *pixels = nullptr;
  int width, height;

  Bitmap(int w, int h) : width(w), height(h) {
    if (!dc || w <= 0 || h <= 0) return;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void *storage = nullptr;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &storage, nullptr, 0);
    if (!bitmap || !storage) return;
    previous = SelectObject(dc, bitmap);
    if (previous && previous != HGDI_ERROR) pixels = static_cast<DWORD *>(storage);
  }

  ~Bitmap() {
    if (dc && pixels) SelectObject(dc, previous);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
  }

  void clear() { std::fill_n(pixels, static_cast<size_t>(width) * height, 0); }
  DWORD &at(int x, int y) { return pixels[static_cast<size_t>(y) * width + x]; }
};

using Icon = std::unique_ptr<std::remove_pointer_t<HICON>, decltype(&DestroyIcon)>;
using Pidl = std::unique_ptr<std::remove_pointer_t<PIDLIST_ABSOLUTE>, decltype(&CoTaskMemFree)>;
using Name = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>;
using Thumb = std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)>;
struct Item {
  Icon icon{nullptr, DestroyIcon};
  Icon overlay{nullptr, DestroyIcon};
  Icon preview{nullptr, DestroyIcon};
  std::wstring label;
  std::uint64_t lastUsed = 0;
  bool thumbnailPending = false;
  bool thumbnailLoaded = false;
};

struct ThumbnailBatch {
  std::atomic_bool canceled = false;
  std::atomic_bool finished = false;
  std::mutex mutex;
  std::vector<std::pair<std::wstring, Thumb>> ready;
};

Pidl parsePath(const std::wstring &path) {
  PIDLIST_ABSOLUTE raw = nullptr;
  SHParseDisplayName(path.c_str(), nullptr, &raw, 0, nullptr);
  return Pidl(raw, CoTaskMemFree);
}

Item loadItem(const std::wstring &path, int iconSize) {
  Item item;
  const auto id = parsePath(path);
  SHFILEINFOW shell{};
  if (id) {
    PWSTR rawLabel = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(id.get(), SIGDN_NORMALDISPLAY, &rawLabel))) {
      const Name name(rawLabel, CoTaskMemFree);
      if (name) item.label = name.get();
    }
    const auto parsed = reinterpret_cast<PCWSTR>(id.get());
    if (SHGetFileInfoW(parsed, 0, &shell, sizeof(shell),
                       SHGFI_PIDL | SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_ADDOVERLAYS | SHGFI_OVERLAYINDEX)) {
      item.icon.reset(shell.hIcon);
      const auto index = static_cast<unsigned>(shell.iIcon);
      const auto listSize = iconSize <= 16 ? SHIL_SMALL : iconSize <= 32 ? SHIL_LARGE :
                            iconSize <= 64 ? SHIL_EXTRALARGE : SHIL_JUMBO;
      Microsoft::WRL::ComPtr<IImageList> images;
      HICON icon = nullptr;
      if (SUCCEEDED(SHGetImageList(listSize, IID_PPV_ARGS(&images)))) {
        const auto overlay = index >> 24;
        if (SUCCEEDED(images->GetIcon(index & 0x00ffffff, ILD_TRANSPARENT | INDEXTOOVERLAYMASK(overlay), &icon)))
          item.icon.reset(icon);
        int overlayImage = 0;
        if (overlay && SUCCEEDED(images->GetOverlayImage(overlay, &overlayImage)) &&
            SUCCEEDED(images->GetIcon(overlayImage, ILD_TRANSPARENT, &icon)))
          item.overlay.reset(icon);
      }
    }
    if (!item.icon && SHGetFileInfoW(parsed, 0, &shell, sizeof(shell), SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON))
      item.icon.reset(shell.hIcon);
  }
  if (item.label.empty()) item.label = std::filesystem::path(path).filename().wstring();
  if (!item.icon) {
    SHSTOCKICONINFO generic{sizeof(generic)};
    if (SUCCEEDED(SHGetStockIconInfo(SIID_DOCNOASSOC, SHGSI_ICON | SHGSI_LARGEICON, &generic)))
      item.icon.reset(generic.hIcon);
  }
  return item;
}

struct Placement {
  std::wstring path;
  POINT center;
  RECT icon{}, label{}, tile{};
};

constexpr UINT textFormat = DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX;

void over(DWORD &destination, BYTE alpha, COLORREF color) {
  if (!alpha) return;
  const unsigned inverse = 255 - alpha;
  const auto mix = [&](unsigned shift, unsigned channel) {
    return ((channel * alpha + ((destination >> shift) & 255) * inverse) / 255) << shift;
  };
  destination = ((alpha + ((destination >> 24) & 255) * inverse / 255) << 24) |
                mix(16, GetRValue(color)) | mix(8, GetGValue(color)) | mix(0, GetBValue(color));
}

BYTE textCoverage(DWORD pixel) {
  return static_cast<BYTE>(((pixel >> 16 & 255) + (pixel >> 8 & 255) + (pixel & 255)) / 3);
}

void drawLabel(Bitmap &frame, Bitmap &mask, const RECT &area, const std::wstring &text, int shadow, COLORREF textColor) {
  mask.clear();
  RECT bounds{shadow, 0, area.right - area.left - shadow, area.bottom - area.top};
  DrawTextW(mask.dc, text.c_str(), static_cast<int>(text.size()), &bounds, textFormat);
  GdiFlush();
  if (textColor == RGB(255, 255, 255)) {
    for (int y = 0; y < bounds.bottom - shadow; ++y) {
      for (int x = bounds.left; x < bounds.right; ++x) {
        const auto alpha = textCoverage(mask.at(x, y));
        over(frame.at(area.left + x, area.top + y + shadow), static_cast<BYTE>(alpha * 3 / 4), 0);
      }
    }
  }
  for (int y = 0; y < bounds.bottom; ++y)
    for (int x = bounds.left; x < bounds.right; ++x) {
      over(frame.at(area.left + x, area.top + y), textCoverage(mask.at(x, y)), textColor);
    }
}

void drawTile(Bitmap &frame, const RECT &rect, int radius, BYTE alpha, COLORREF color) {
  if (!alpha) return;
  const int radiusSquared = radius * radius;
  for (LONG y = rect.top; y < rect.bottom; ++y) {
    const int dy = std::max({static_cast<int>(rect.top + radius - y - 1),
                             static_cast<int>(y - rect.bottom + radius), 0});
    for (LONG x = rect.left; x < rect.right; ++x) {
      const int dx = std::max({static_cast<int>(rect.left + radius - x - 1),
                               static_cast<int>(x - rect.right + radius), 0});
      if (dx * dx + dy * dy <= radiusSquared) over(frame.at(x, y), alpha, color);
    }
  }
}

void readIcon(Bitmap &tile, HICON icon) {
  tile.clear();
  if (!icon) return;
  if (!DrawIconEx(tile.dc, 0, 0, icon, tile.width, tile.height, 0, nullptr, DI_NORMAL)) return;
  GdiFlush();
  const auto pixelCount = static_cast<size_t>(tile.width) * tile.height;
  const bool alpha = std::any_of(tile.pixels, tile.pixels + pixelCount, [](DWORD color) { return color >> 24; });
  if (!alpha) {
    // Classic icons supply a separate mask instead of an alpha channel.
    Bitmap mask(tile.width, tile.height);
    if (mask.pixels && DrawIconEx(mask.dc, 0, 0, icon, mask.width, mask.height, 0, nullptr, DI_MASK)) {
      GdiFlush();
      for (size_t i = 0; i < pixelCount; ++i)
        if ((mask.pixels[i] & 0xffffff) == 0) tile.pixels[i] |= 0xff000000;
    }
  }
}

void drawIcon(Bitmap &frame, Bitmap &tile, const RECT &area, HICON icon) {
  readIcon(tile, icon);
  for (int y = 0; y < tile.height; ++y)
    for (int x = 0; x < tile.width; ++x)
      if (const auto color = tile.at(x, y); color >> 24) {
        auto &destination = frame.at(area.left + x, area.top + y);
        const auto inverse = 255 - (color >> 24);
        const auto channel = [&](int shift) {
          return std::min<DWORD>(255, (color >> shift & 255) + (destination >> shift & 255) * inverse / 255) << shift;
        };
        destination = channel(24) | channel(16) | channel(8) | channel(0);
      }
}

Icon thumbnailIcon(HBITMAP image, int size, HICON overlay) {
  Icon result(nullptr, DestroyIcon);
  BITMAP dimensions{};
  if (!GetObjectW(image, sizeof(dimensions), &dimensions) || dimensions.bmWidth <= 0 ||
      dimensions.bmHeight <= 0 || dimensions.bmWidth > size || dimensions.bmHeight > size) return result;
  Bitmap source(dimensions.bmWidth, dimensions.bmHeight), tile(size, size);
  if (!source.pixels || !tile.pixels) return result;
  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), source.width, -source.height, 1, 32, BI_RGB};
  if (GetDIBits(source.dc, image, 0, source.height, source.pixels, &info, DIB_RGB_COLORS) != source.height) return result;
  const auto count = static_cast<size_t>(source.width) * source.height;
  const bool alpha = std::any_of(source.pixels, source.pixels + count, [](DWORD pixel) { return pixel >> 24; });
  tile.clear();
  const int left = (size - source.width) / 2, top = (size - source.height) / 2;
  for (int y = 0; y < source.height; ++y) for (int x = 0; x < source.width; ++x) {
    const auto pixel = source.at(x, y);
    tile.at(left + x, top + y) = alpha ? pixel : pixel | 0xff000000;
  }
  if (overlay) {
    Bitmap badge(size, size);
    if (!badge.pixels) return result;
    readIcon(badge, overlay);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
      const auto pixel = badge.at(x, y);
      const auto opacity = static_cast<BYTE>(pixel >> 24);
      if (!opacity) continue;
      const auto straight = [&](unsigned shift) { return std::min<DWORD>(255, ((pixel >> shift) & 255) * 255 / opacity); };
      over(tile.at(x, y), opacity, RGB(straight(16), straight(8), straight(0)));
    }
  }
  const std::vector<BYTE> maskBits(static_cast<size_t>((size + 15) / 16) * 2 * size);
  const Thumb mask(CreateBitmap(size, size, 1, 1, maskBits.data()), DeleteObject);
  if (!mask) return result;
  ICONINFO icon{TRUE, 0, 0, mask.get(), tile.bitmap};
  SelectObject(tile.dc, tile.previous);
  tile.pixels = nullptr;
  result.reset(CreateIconIndirect(&icon));
  return result;
}

void openItem(HWND owner, const std::wstring &path) {
  const auto id = parsePath(path);
  if (!id) {
    ShellExecuteW(owner, nullptr, path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  SHELLEXECUTEINFOW execute{sizeof(execute)};
  execute.fMask = SEE_MASK_IDLIST;
  execute.hwnd = owner;
  execute.lpIDList = id.get();
  execute.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&execute);
}

const Placement *hit(const std::vector<Placement> &placements, POINT screen) {
  for (auto it = placements.rbegin(); it != placements.rend(); ++it)
    if (PtInRect(&it->tile, screen)) return &*it;
  return nullptr;
}

void addRegion(HRGN region, const RECT &area, POINT origin) {
  if (const auto part = CreateRectRgn(area.left - origin.x, area.top - origin.y,
                                    area.right - origin.x, area.bottom - origin.y)) {
    CombineRgn(region, region, part, RGN_OR);
    DeleteObject(part);
  }
}

LRESULT CALLBACK marqueeProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCHITTEST) return HTTRANSPARENT;
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    RECT area{};
    GetClientRect(window, &area);
    const auto accent = GetSysColor(COLOR_HIGHLIGHT);
    const auto brush = reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, accent);
    FillRect(dc, &area, brush);
    SetDCBrushColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    FrameRect(dc, &area, brush);
    EndPaint(window, &paint);
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
}

struct DesktopIcons::State {
  HWND window = nullptr;
  DesktopIconEvents events;
  DesktopIconMetrics metrics;
  std::vector<Placement> placements;
  std::unordered_map<std::wstring, Item> icons;
  std::shared_ptr<ThumbnailBatch> thumbnails;
  std::unordered_set<std::wstring> selected;
  std::unordered_set<std::wstring> selectionBeforeDrag;
  std::wstring hovered;
  std::wstring pressed;
  Icon pressedImage{nullptr, DestroyIcon};
  RECT pressedBounds{};
  POINT start{};
  POINT selectionOrigin{};
  RECT selectionArea{};
  HWND marquee = nullptr;
  HHOOK desktopInput = nullptr;
  bool pendingBlankPress = false, selecting = false, togglingSelection = false;
  bool pressedWithoutToggle = false;
  static inline thread_local State *inputOwner = nullptr;
  std::uint64_t frameNumber = 0;
  bool dirty = true;

  explicit State(HINSTANCE instance, DesktopIconEvents callbacks) : events(std::move(callbacks)) {
    WNDCLASSW cls{};
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = procedure;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = hostClass;
    if (RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
      window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, hostClass, nullptr,
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (window && !inputOwner) {
      inputOwner = this;
      desktopInput = SetWindowsHookExW(WH_MOUSE_LL, observeDesktopInput, instance, 0);
      if (!desktopInput) inputOwner = nullptr;
    }
  }

  ~State() {
    if (desktopInput) UnhookWindowsHookEx(desktopInput);
    if (inputOwner == this) inputOwner = nullptr;
    if (marquee) DestroyWindow(marquee);
    if (thumbnails) thumbnails->canceled = true;
    if (window) DestroyWindow(window);
  }

  static bool nativeDesktopAt(HWND hit, HWND root) {
    if (quick::isDesktop(root)) return true;
    // Explorer may expose a wallpaper-only WorkerW after Show Desktop.
    if (!root || hit != root) return false;
    DWORD shellPid = 0, pid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    GetWindowThreadProcessId(root, &pid);
    wchar_t name[32]{};
    return shellPid && pid == shellPid && GetClassNameW(root, name, ARRAYSIZE(name)) &&
           wcscmp(name, L"WorkerW") == 0;
  }

  static LRESULT CALLBACK observeDesktopInput(int code, WPARAM message, LPARAM data) {
    auto *self = inputOwner;
    if (code != HC_ACTION || !self) return CallNextHookEx(nullptr, code, message, data);
    const auto &mouse = *reinterpret_cast<const MSLLHOOKSTRUCT *>(data);
    const auto postPoint = [&](UINT event) {
      return PostMessageW(self->window, event, static_cast<WPARAM>(static_cast<INT_PTR>(mouse.pt.x)), mouse.pt.y);
    };
    if (message == WM_LBUTTONUP && self->events.dragReleased) self->events.dragReleased(mouse.pt);
    if (self->pendingBlankPress) {
      if (message == WM_LBUTTONUP) {
        self->pendingBlankPress = false;
        postPoint(selectionFinish);
      } else if (message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN) {
        self->pendingBlankPress = false;
        PostMessageW(self->window, selectionCancel, 0, 0);
      }
    }
    const bool wheel = message == WM_MOUSEWHEEL && self->events.zoom && !self->selecting &&
                       !self->pendingBlankPress && self->pressed.empty() && GetAsyncKeyState(VK_CONTROL) < 0;
    const bool desktopMenu = message == WM_RBUTTONDOWN && self->events.desktopMenuOpened;
    if (message == WM_LBUTTONDOWN || wheel || desktopMenu) {
      GUITHREADINFO gui{sizeof(gui)};
      if (GetGUIThreadInfo(0, &gui) && !gui.hwndCapture &&
          !(gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_INMOVESIZE))) {
        const auto hit = WindowFromPoint(mouse.pt);
        const auto root = GetAncestor(hit, GA_ROOT);
        if (root == self->window || nativeDesktopAt(hit, root)) {
          if (wheel) {
            const auto delta = static_cast<SHORT>(HIWORD(mouse.mouseData));
            if (PostMessageW(self->window, desktopWheel, static_cast<WPARAM>(static_cast<INT_PTR>(delta)), 0)) return 1;
          } else if (desktopMenu) {
            if (root != self->window) self->events.desktopMenuOpened(mouse.pt);
          } else if (root != self->window) {
            self->pendingBlankPress = postPoint(GetAsyncKeyState(VK_CONTROL) < 0 ? selectionToggle : selectionStart) != FALSE;
          }
        }
      }
    }
    return CallNextHookEx(nullptr, code, message, data);
  }

  void startSelection(POINT point, bool toggle) {
    if (!pressed.empty() || placements.empty()) return;
    selectionBeforeDrag = selected;
    selectionOrigin = point;
    selectionArea = {};
    togglingSelection = toggle;
    selecting = SetTimer(window, selectionTimer, 16, nullptr) != 0;
    if (!selecting) selectionBeforeDrag.clear();
  }

  void moveSelection(POINT point) {
    if (!selecting || (IsRectEmpty(&selectionArea) &&
        std::abs(point.x - selectionOrigin.x) < GetSystemMetricsForDpi(SM_CXDRAG, metrics.dpi) &&
        std::abs(point.y - selectionOrigin.y) < GetSystemMetricsForDpi(SM_CYDRAG, metrics.dpi))) return;
    const RECT next{std::min(point.x, selectionOrigin.x), std::min(point.y, selectionOrigin.y),
                    std::max(point.x, selectionOrigin.x) + 1, std::max(point.y, selectionOrigin.y) + 1};
    if (EqualRect(&selectionArea, &next)) return;
    selectionArea = next;
    auto items = togglingSelection ? selectionBeforeDrag : decltype(selected){};
    for (const auto &entry : placements) {
      RECT overlap{};
      if (!IntersectRect(&overlap, &next, &entry.tile)) continue;
      if (!items.erase(entry.path)) items.insert(entry.path);
    }
    if (!marquee) {
      const auto instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
      WNDCLASSW cls{};
      cls.lpfnWndProc = marqueeProc;
      cls.hInstance = instance;
      cls.lpszClassName = marqueeClass;
      if (RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        marquee = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                  marqueeClass, nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
      if (marquee) SetLayeredWindowAttributes(marquee, 0, 72, LWA_ALPHA);
    }
    if (marquee)
      SetWindowPos(marquee, window, next.left, next.top, next.right - next.left, next.bottom - next.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (items != selected) {
      selected = std::move(items);
      redraw();
    }
  }

  void finishSelection(bool cancel) {
    if (!std::exchange(selecting, false)) return;
    KillTimer(window, selectionTimer);
    if (marquee) ShowWindow(marquee, SW_HIDE);
    const bool changed = cancel ? selected != selectionBeforeDrag :
                         IsRectEmpty(&selectionArea) && !togglingSelection && !selected.empty();
    if (changed) {
      if (cancel) selected = std::move(selectionBeforeDrag);
      else selected.clear();
      redraw();
    }
    selectionBeforeDrag.clear();
    selectionArea = {};
  }

  void stopDrag() {
    KillTimer(window, dragTimer);
    pressedImage.reset();
    const bool wasPressed = !std::exchange(pressed, std::wstring{}).empty();
    if (GetCapture() == window) ReleaseCapture();
    if (wasPressed) redraw();
  }

  void redraw() {
    dirty = true;
    std::vector<LooseItem> items;
    items.reserve(placements.size());
    for (const auto &entry : placements) items.push_back({entry.path, entry.center});
    update(items, metrics);
  }

  void queueThumbnails() {
    if (thumbnails || placements.empty()) return;
    std::vector<std::wstring> paths;
    for (const auto &entry : placements) {
      auto &item = icons.at(entry.path);
      if (item.thumbnailPending || item.thumbnailLoaded) continue;
      paths.push_back(entry.path);
      item.thumbnailPending = true;
    }
    if (paths.empty()) return;
    auto batch = std::make_shared<ThumbnailBatch>();
    if (!SetTimer(window, thumbnailTimer, 80, nullptr)) {
      for (const auto &path : paths) icons.at(path).thumbnailPending = false;
      return;
    }
    try {
      std::thread([batch, paths = std::move(paths), pixels = metrics.iconSize] {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
          try {
            for (const auto &path : paths) {
              if (batch->canceled) break;
              const auto attributes = GetFileAttributesW(path.c_str());
              Thumb image(nullptr, DeleteObject);
              if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
                if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) {
                  HBITMAP result = nullptr;
                  const auto status = factory->GetImage({pixels, pixels}, SIIGBF_THUMBNAILONLY | SIIGBF_SCALEUP, &result);
                  image.reset(result);
                  if (FAILED(status)) image.reset();
                }
              }
              std::lock_guard lock(batch->mutex);
              batch->ready.emplace_back(path, std::move(image));
            }
          } catch (...) {}
          CoUninitialize();
        }
        batch->finished = true;
      }).detach();
      thumbnails = std::move(batch);
    } catch (...) {
      KillTimer(window, thumbnailTimer);
      for (auto &entry : icons) entry.second.thumbnailPending = false;
    }
  }

  void cancelThumbnails() {
    if (!thumbnails) return;
    thumbnails->canceled = true;
    thumbnails.reset();
    KillTimer(window, thumbnailTimer);
  }

  void applyThumbnails() {
    if (!thumbnails) return;
    const bool finished = thumbnails->finished;
    decltype(thumbnails->ready) ready;
    {
      std::lock_guard lock(thumbnails->mutex);
      ready.swap(thumbnails->ready);
    }
    bool changed = false;
    for (const auto &[path, image] : ready) {
      const auto found = icons.find(path);
      if (found == icons.end()) continue;
      auto &item = found->second;
      item.thumbnailPending = false;
      item.thumbnailLoaded = true;
      if (image) {
        item.preview = thumbnailIcon(image.get(), metrics.iconSize, item.overlay.get());
        changed = changed || static_cast<bool>(item.preview);
      }
    }
    if (finished) {
      cancelThumbnails();
      for (auto &entry : icons)
        if (std::exchange(entry.second.thumbnailPending, false)) entry.second.thumbnailLoaded = true;
    }
    if (changed) redraw();
    else if (finished) queueThumbnails();
  }

  void select(const std::wstring &path, bool toggle) {
    if (!toggle && selected.size() == 1 && selected.contains(path)) return;
    if (!toggle || selected.erase(path) == 0) {
      if (!toggle) selected.clear();
      selected.insert(path);
    }
    redraw();
  }

  void testDrag() {
    if (pressed.empty()) return;
    POINT screen{};
    if (!GetCursorPos(&screen) || GetAsyncKeyState(VK_LBUTTON) >= 0) {
      stopDrag();
      return;
    }
    if (std::abs(screen.x - start.x) < GetSystemMetricsForDpi(SM_CXDRAG, metrics.dpi) &&
        std::abs(screen.y - start.y) < GetSystemMetricsForDpi(SM_CYDRAG, metrics.dpi)) return;
    std::vector<std::wstring> paths{pressed};
    if (selected.contains(pressed)) {
      for (const auto &entry : placements)
        if (entry.path != pressed && selected.contains(entry.path)) paths.push_back(entry.path);
    }
    auto preview = std::move(pressedImage);
    const auto bounds = pressedBounds;
    const auto origin = start;
    stopDrag();
    if (!events.beginDrag) return;
    events.beginDrag(window, std::move(paths), preview.release(), bounds, origin);
  }

  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
      const auto create = reinterpret_cast<const CREATESTRUCTW *>(l);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto *self = reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, w, l);
    try {
      switch (message) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_ERASEBKGND:
        return 1;
      case WM_DPICHANGED:
      case WM_DISPLAYCHANGE:
      case WM_SETTINGCHANGE:
        self->dirty = true;
        break;
      case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->window = nullptr;
        break;
      case selectionStart:
      case selectionToggle:
        self->startSelection({static_cast<LONG>(static_cast<INT_PTR>(w)), static_cast<LONG>(l)}, message == selectionToggle);
        return 0;
      case selectionFinish:
        self->moveSelection({static_cast<LONG>(static_cast<INT_PTR>(w)), static_cast<LONG>(l)});
        self->finishSelection(false);
        return 0;
      case selectionCancel:
        self->finishSelection(true);
        return 0;
      case desktopWheel:
        if (self->events.zoom) self->events.zoom(static_cast<int>(static_cast<INT_PTR>(w)));
        return 0;
      case WM_TIMER:
        if (w == dragTimer) {
          self->testDrag();
          return 0;
        }
        if (w == thumbnailTimer) {
          self->applyThumbnails();
          return 0;
        }
        if (w == selectionTimer) {
          POINT point{};
          if (GetAsyncKeyState(VK_ESCAPE) < 0) self->finishSelection(true);
          else if (!self->pendingBlankPress || GetAsyncKeyState(VK_LBUTTON) >= 0) {
            self->pendingBlankPress = false;
            if (GetCursorPos(&point)) self->moveSelection(point);
            self->finishSelection(false);
          } else if (GetCursorPos(&point)) self->moveSelection(point);
          return 0;
        }
        break;
      case WM_MOUSEWHEEL:
        if ((GET_KEYSTATE_WPARAM(w) & MK_CONTROL) && self->events.zoom && !self->selecting &&
            !self->pendingBlankPress && self->pressed.empty()) {
          self->events.zoom(GET_WHEEL_DELTA_WPARAM(w));
          return 0;
        }
        break;
      case WM_CANCELMODE:
        self->finishSelection(true);
        self->stopDrag();
        return 0;
      case WM_CAPTURECHANGED:
        self->stopDrag();
        return 0;
      case WM_LBUTTONUP:
        if (!self->pressed.empty() && self->pressedWithoutToggle) self->select(self->pressed, false);
        self->stopDrag();
        return 0;
      case WM_MOUSEMOVE:
      case WM_MOUSELEAVE: {
        std::wstring path;
        if (message == WM_MOUSEMOVE) {
          if (!self->pressed.empty()) {
            self->testDrag();
            if (self->pressed.empty()) return 0;
          }
          TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
          TrackMouseEvent(&track);
          POINT screen{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
          if (ClientToScreen(window, &screen))
            if (const auto *entry = hit(self->placements, screen)) path = entry->path;
        }
        if (self->hovered != path) {
          self->hovered = std::move(path);
          self->redraw();
        }
        return 0;
      }
      case WM_LBUTTONDOWN:
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONUP:
      case WM_CONTEXTMENU: {
        POINT screen{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (message != WM_CONTEXTMENU) ClientToScreen(window, &screen);
        auto *item = hit(self->placements, screen);
        if (message == WM_CONTEXTMENU && screen.x == -1 && screen.y == -1) {
          const auto selected = std::find_if(self->placements.begin(), self->placements.end(),
              [&](const auto &entry) { return self->selected.contains(entry.path); });
          if (selected != self->placements.end()) {
            item = &*selected;
            screen = {item->label.left, item->label.bottom};
          }
        }
        if (!item) break;
        if ((message == WM_RBUTTONUP || message == WM_CONTEXTMENU) && self->events.contextMenu) {
          const auto path = item->path;
          if (!self->selected.contains(path)) self->select(path, false);
          self->events.contextMenu(window, path, screen);
        } else if (message == WM_LBUTTONDBLCLK) {
          const auto path = item->path;
          self->stopDrag();
          openItem(window, path);
        } else if (message == WM_LBUTTONDOWN) {
          const auto path = item->path;
          const auto bounds = item->icon;
          self->finishSelection(true);
          self->pressed = path;
          self->pressedBounds = bounds;
          const auto &cached = self->icons.at(path);
          const auto image = cached.preview ? cached.preview.get() : cached.icon.get();
          self->pressedImage.reset(image ? CopyIcon(image) : nullptr);
          self->start = screen;
          self->pressedWithoutToggle = !(w & MK_CONTROL);
          if (!self->pressedWithoutToggle || !self->selected.contains(self->pressed))
            self->select(self->pressed, !self->pressedWithoutToggle);
          SetCapture(window);
          SetTimer(window, dragTimer, 10, nullptr);
        }
        return 0;
      }
      }
    } catch (...) {
      self->finishSelection(true);
      self->stopDrag();
    }
    return DefWindowProcW(window, message, w, l);
  }

  bool update(const std::vector<LooseItem> &items, DesktopIconMetrics next) {
    if (!window) return false;
    if (items.empty()) {
      cancelThumbnails();
      placements.clear();
      icons.clear();
      selected.clear();
      hovered.clear();
      finishSelection(false);
      stopDrag();
      ShowWindow(window, SW_HIDE);
      return true;
    }
    next.iconSize = std::clamp(next.iconSize, 8, 256);
    next.dpi = std::clamp<UINT>(next.dpi, 48, 768);
    next.spacing.cx = std::clamp<LONG>(next.spacing.cx, next.iconSize, 2048);
    next.spacing.cy = std::clamp<LONG>(next.spacing.cy, next.iconSize, 2048);
    const bool changedSize = metrics.iconSize != next.iconSize;
    if (!dirty && !changedSize && metrics.dpi == next.dpi && metrics.spacing.cx == next.spacing.cx &&
        metrics.spacing.cy == next.spacing.cy && std::memcmp(&metrics.labelFont, &next.labelFont, sizeof(LOGFONTW)) == 0 &&
        placements.size() == items.size() &&
        std::equal(items.begin(), items.end(), placements.begin(), [](const auto &a, const auto &b) {
          return a.path == b.path && a.position.x == b.center.x && a.position.y == b.center.y;
        })) return true;
    if (changedSize) {
      cancelThumbnails();
      icons.clear();
    }
    ++frameNumber;

    const auto px = [&](int value) { return MulDiv(value, next.dpi, 96); };
    auto fontInfo = next.labelFont;
    if (!fontInfo.lfHeight &&
        !SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(fontInfo), &fontInfo, 0, next.dpi)) {
      fontInfo.lfHeight = -px(12);
      wcscpy_s(fontInfo.lfFaceName, L"Segoe UI");
    }
    const std::unique_ptr<std::remove_pointer_t<HFONT>, decltype(&DeleteObject)> font(
        CreateFontIndirectW(&fontInfo), DeleteObject);
    const int shadow = std::max(1, px(1));
    const int labelWidth = std::max({next.iconSize, static_cast<int>(next.spacing.cx) - px(8), 3 * shadow});
    SIZE line{};
    if (const auto screen = GetDC(nullptr)) {
      const auto previous = SelectObject(screen, font.get());
      GetTextExtentPoint32W(screen, L"Ag", 2, &line);
      if (previous) SelectObject(screen, previous);
      ReleaseDC(nullptr, screen);
    }
    const int labelHeight = std::max<LONG>(px(12), line.cy) * 2 + shadow;
    Bitmap mask(labelWidth, labelHeight), tile(next.iconSize, next.iconSize);
    if (!font || !mask.pixels || !tile.pixels) return false;
    const auto oldFont = SelectObject(mask.dc, font.get());
    SetTextColor(mask.dc, RGB(255, 255, 255));
    SetBkMode(mask.dc, TRANSPARENT);

    RECT desktop{};
    std::vector<Placement> prepared;
    prepared.reserve(items.size());
    for (const auto &item : items) {
      if (!MonitorFromPoint(item.position, MONITOR_DEFAULTTONULL)) {
        SelectObject(mask.dc, oldFont);
        return false;
      }
      auto cached = icons.find(item.path);
      if (cached == icons.end()) cached = icons.emplace(item.path, loadItem(item.path, next.iconSize)).first;
      cached->second.lastUsed = frameNumber;
      Placement entry{item.path, item.position};
      const int x = entry.center.x - next.iconSize / 2;
      const int y = entry.center.y - next.spacing.cy / 2 + px(6);
      entry.icon = {x, y, x + next.iconSize, y + next.iconSize};
      RECT measured{0, 0, labelWidth - shadow * 2, labelHeight};
      const auto &text = cached->second.label;
      DrawTextW(mask.dc, text.c_str(), static_cast<int>(text.size()), &measured, textFormat | DT_CALCRECT);
      const int width = std::clamp<LONG>(measured.right + shadow * 2, 1, labelWidth);
      const int top = entry.icon.bottom + px(4);
      entry.label = {entry.center.x - width / 2, top, entry.center.x - width / 2 + width,
                     top + std::clamp<LONG>(measured.bottom + shadow, shadow + 1, labelHeight)};
      UnionRect(&entry.tile, &entry.icon, &entry.label);
      InflateRect(&entry.tile, px(4), px(2));
      UnionRect(&desktop, &desktop, &entry.tile);
      prepared.push_back(std::move(entry));
    }

    Bitmap frame(desktop.right - desktop.left, desktop.bottom - desktop.top);
    const auto region = CreateRectRgn(0, 0, 0, 0);
    if (!frame.pixels || !region) {
      if (region) DeleteObject(region);
      SelectObject(mask.dc, oldFont);
      return false;
    }
    frame.clear();
    POINT origin{desktop.left, desktop.top};
    const auto colors = ui::palette(ui::configuredTheme());
    for (const auto &entry : prepared) {
      addRegion(region, entry.tile, origin);
      RECT icon = entry.icon, label = entry.label, area = entry.tile;
      OffsetRect(&icon, -origin.x, -origin.y);
      OffsetRect(&label, -origin.x, -origin.y);
      OffsetRect(&area, -origin.x, -origin.y);
      const auto &item = icons.at(entry.path);
      const bool active = selected.contains(entry.path);
      const bool hot = entry.path == hovered, held = entry.path == pressed;
      const BYTE alpha = active ? (held ? 49 : hot ? 42 : 32)
                               : held ? (colors.dark ? 10 : 6) : hot ? (colors.dark ? 15 : 9) : 0;
      drawTile(frame, area, px(4), colors.highContrast && (active || hot) ? 255 : alpha,
               colors.highContrast ? colors.accent : active ? colors.text : colors.dark ? RGB(255, 255, 255) : 0);
      drawIcon(frame, tile, icon, item.preview ? item.preview.get() : item.icon.get());
      drawLabel(frame, mask, label, item.label, shadow,
                colors.highContrast && (active || hot) ? colors.onAccent : RGB(255, 255, 255));
      // Keep each tile interactive so it can receive the first hover event.
      for (int y = area.top; y < area.bottom; ++y)
        for (int x = area.left; x < area.right; ++x)
          if (!frame.at(x, y)) frame.at(x, y) = 0x01000000;
    }
    SelectObject(mask.dc, oldFont);
    SIZE size{frame.width, frame.height};
    POINT source{};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(window, nullptr, &origin, &size, frame.dc, &source, 0, &blend, ULW_ALPHA)) {
      DeleteObject(region);
      return false;
    }
    if (!SetWindowRgn(window, region, TRUE)) {
      DeleteObject(region);
      return false;
    }
    metrics = next;
    placements = std::move(prepared);
    dirty = false;
    std::erase_if(icons, [&](const auto &cached) { return cached.second.lastUsed != frameNumber; });
    std::erase_if(selected, [&](const auto &path) { return !icons.contains(path); });
    if (!IsWindowVisible(window)) ShowWindow(window, SW_SHOWNOACTIVATE);
    queueThumbnails();
    return IsWindowVisible(window) != FALSE;
  }
};

DesktopIcons::DesktopIcons(HINSTANCE instance, DesktopIconEvents events)
    : state(std::make_unique<State>(instance, std::move(events))) {}
DesktopIcons::~DesktopIcons() = default;
HWND DesktopIcons::window() const { return state->window; }
std::wstring DesktopIcons::itemAt(POINT screen) const {
  const auto *item = hit(state->placements, screen);
  return item ? item->path : std::wstring{};
}
std::vector<LooseItem> DesktopIcons::positions() const {
  std::vector<LooseItem> items;
  items.reserve(state->placements.size());
  for (const auto &entry : state->placements) items.push_back({entry.path, entry.center});
  return items;
}
bool DesktopIcons::update(const std::vector<LooseItem> &items, DesktopIconMetrics metrics) {
  return state->update(items, metrics);
}
}
