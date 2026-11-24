// Drag and drop between boxes, the desktop and Explorer, plus transfer progress.
#include "desktop_groups_internal.h"

namespace desktop_groups::detail {
std::vector<std::wstring> desktopDropPaths(IDataObject *data) {
  auto paths = desktopShellDropItems(data);
  FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  struct Medium {
    STGMEDIUM data{};
    ~Medium() { if (data.tymed != TYMED_NULL) ReleaseStgMedium(&data); }
  } medium;
  if (SUCCEEDED(data->GetData(&format, &medium.data)) && medium.data.tymed == TYMED_HGLOBAL) {
    const auto files = static_cast<HDROP>(medium.data.hGlobal);
    const auto count = DragQueryFileW(files, 0xffffffff, nullptr, 0);
    bool readFile = false;
    for (UINT i = 0; i < count; ++i) {
      std::wstring path(static_cast<size_t>(DragQueryFileW(files, i, nullptr, 0)) + 1, L'\0');
      path.resize(DragQueryFileW(files, i, path.data(), static_cast<UINT>(path.size())));
      if (path.empty()) continue;
      if (!readFile) {
        // Prefer the explicit file paths when the source provides them. A
        // Shell list may use a second spelling of the same files.
        std::erase_if(paths, [](const auto &item) { return !isDesktopShellItem(item); });
        readFile = true;
      }
      paths.push_back(std::move(path));
    }
  }
  return paths;
}

void setDropEffect(IDataObject *data, const wchar_t *name, DWORD effect) {
  const auto formatId = RegisterClipboardFormatW(name);
  if (!formatId) return;
  STGMEDIUM medium{};
  medium.tymed = TYMED_HGLOBAL;
  medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(effect));
  if (!medium.hGlobal) return;
  if (auto *value = static_cast<DWORD *>(GlobalLock(medium.hGlobal))) {
    *value = effect;
    GlobalUnlock(medium.hGlobal);
    FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (SUCCEEDED(data->SetData(&format, &medium, TRUE))) return;
  }
  GlobalFree(medium.hGlobal);
}

std::optional<DWORD> readDropEffect(IDataObject *data, const wchar_t *name) {
  if (!data) return std::nullopt;
  const auto formatId = RegisterClipboardFormatW(name);
  if (!formatId) return std::nullopt;
  FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format, &medium)) || medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return std::nullopt;
  std::optional<DWORD> result;
  if (GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
    if (const auto *value = static_cast<const DWORD *>(GlobalLock(medium.hGlobal))) {
      result = *value;
      GlobalUnlock(medium.hGlobal);
    }
  }
  ReleaseStgMedium(&medium);
  return result;
}

std::wstring recycleBinDisplayName() {
  Microsoft::WRL::ComPtr<IShellItem> item;
  PWSTR raw = nullptr;
  if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&item))) &&
      item && SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &raw)) && raw) {
    std::wstring name(raw);
    CoTaskMemFree(raw);
    if (!name.empty()) return name;
  }
  return std::wstring(lang::text(L"回收站"));
}

std::optional<CLSID> readTargetClsid(IDataObject *data) {
  if (!data) return std::nullopt;
  const auto formatId = RegisterClipboardFormatW(CFSTR_TARGETCLSID);
  if (!formatId) return std::nullopt;
  FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&format, &medium)) || medium.tymed != TYMED_HGLOBAL || !medium.hGlobal) return std::nullopt;
  std::optional<CLSID> result;
  if (GlobalSize(medium.hGlobal) >= sizeof(CLSID)) {
    if (const auto *value = static_cast<const CLSID *>(GlobalLock(medium.hGlobal))) {
      result = *value;
      GlobalUnlock(medium.hGlobal);
    }
  }
  ReleaseStgMedium(&medium);
  return result;
}

bool desktopIconAt(HWND view, POINT point) {
  using Microsoft::WRL::ComPtr;
  const HWND under = WindowFromPoint(point);
  if (!view || !IsWindowVisible(view) || !under || !IsWindowVisible(under) ||
      (under != view && !IsChild(view, under))) {
    return false;
  }
  ComPtr<IUIAutomation> ui;
  ComPtr<IUIAutomation2> options;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui))) || !ui ||
      FAILED(ui.As(&options)) || !options || FAILED(options->put_ConnectionTimeout(120)) ||
      FAILED(options->put_TransactionTimeout(120))) {
    return false;
  }
  ComPtr<IUIAutomationElement> root, element;
  ComPtr<IUIAutomationTreeWalker> walker;
  if (FAILED(ui->ElementFromHandle(view, &root)) || !root ||
      FAILED(ui->ElementFromPoint(point, &element)) || !element ||
      FAILED(ui->get_RawViewWalker(&walker)) || !walker) {
    return false;
  }
  for (unsigned depth = 0; element && depth < 12; ++depth) {
    BOOL equal = FALSE;
    if (SUCCEEDED(ui->CompareElements(root.Get(), element.Get(), &equal)) && equal) return false;
    CONTROLTYPEID type{};
    if (FAILED(element->get_CurrentControlType(&type))) return false;
    if (type == UIA_ListItemControlTypeId || type == UIA_DataItemControlTypeId) return true;
    ComPtr<IUIAutomationElement> parent;
    if (FAILED(walker->GetParentElement(element.Get(), &parent))) return false;
    element = std::move(parent);
  }
  return false;
}

Microsoft::WRL::ComPtr<IDataObject> createDragDataObject(HWND owner, std::span<const std::wstring> paths) {
  if (paths.empty()) return nullptr;
  using ItemId = std::unique_ptr<void, decltype(&CoTaskMemFree)>;
  std::vector<ItemId> ownedIds;
  std::vector<PCIDLIST_ABSOLUTE> ids;
  ownedIds.reserve(paths.size());
  ids.reserve(paths.size());
  for (const auto &path : paths) {
    PIDLIST_ABSOLUTE id = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) || !id) continue;
    ownedIds.emplace_back(static_cast<void *>(id), CoTaskMemFree);
    ids.push_back(id);
  }
  if (ids.empty()) return nullptr;
  Microsoft::WRL::ComPtr<IDataObject> dataObject;
  Microsoft::WRL::ComPtr<IShellItemArray> shellItems;
  if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), &shellItems)) && shellItems) {
    shellItems->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dataObject));
  }
  if (!dataObject && ids.size() == 1) {
    Microsoft::WRL::ComPtr<IShellFolder> folder;
    PCUITEMID_CHILD child = nullptr;
    if (SUCCEEDED(SHBindToParent(ids.front(), IID_PPV_ARGS(&folder), &child)) && folder && child) {
      folder->GetUIObjectOf(owner, 1, &child, IID_IDataObject, nullptr,
                            reinterpret_cast<void **>(dataObject.GetAddressOf()));
    }
  }
  if (dataObject) {
    Microsoft::WRL::ComPtr<IDataObjectAsyncCapability> async;
    if (SUCCEEDED(dataObject.As(&async)) && async) async->SetAsyncMode(VARIANT_FALSE);
  }
  return dataObject;
}

void attachDragImage(IDataObject *data, HICON icon, SIZE iconSize, POINT iconAnchor,
                     const std::wstring &label, size_t itemCount, UINT dpi) {
  if (!data || !icon || iconSize.cx <= 0 || iconSize.cy <= 0) return;
  Microsoft::WRL::ComPtr<IDragSourceHelper> helper;
  if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))) || !helper)
    return;
  Microsoft::WRL::ComPtr<IDragSourceHelper2> helper2;
  if (SUCCEEDED(helper.As(&helper2)) && helper2) helper2->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT);

  const bool showLabel = itemCount <= 1 && !label.empty();
  const bool multiStack = itemCount > 1;
  const LONG stackOffset = multiStack ? std::max(4L, static_cast<LONG>(MulDiv(6, dpi, 96))) : 0;
  const LONG textGap = showLabel ? std::max(2L, static_cast<LONG>(MulDiv(4, dpi, 96))) : 0;
  const LONG textHeight = showLabel ? std::max(24L, static_cast<LONG>(MulDiv(34, dpi, 96))) : 0;
  const LONG canvasW = showLabel
      ? std::max<LONG>(iconSize.cx + MulDiv(36, dpi, 96), MulDiv(104, dpi, 96))
      : iconSize.cx + stackOffset * 2;
  const LONG canvasH = showLabel ? (iconSize.cy + textGap + textHeight) : (iconSize.cy + stackOffset * 2);
  const LONG iconLeft = showLabel ? (canvasW - iconSize.cx) / 2 : stackOffset;
  const LONG iconTop = showLabel ? 0 : stackOffset;

  BITMAPINFO info{};
  info.bmiHeader = {sizeof(BITMAPINFOHEADER), canvasW, -canvasH, 1, 32, BI_RGB};
  void *pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  if (!bitmap || !pixels) {
    if (bitmap) DeleteObject(bitmap);
    return;
  }

  const auto drawIconToArgb = [](HICON hIcon, LONG w, LONG h, std::vector<DWORD> &out) -> bool {
    out.assign(static_cast<size_t>(w) * h, 0);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void *buf = nullptr;
    HBITMAP tmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &buf, nullptr, 0);
    if (!tmp || !buf) {
      if (tmp) DeleteObject(tmp);
      return false;
    }
    HDC mem = CreateCompatibleDC(nullptr);
    bool ok = false;
    if (mem) {
      const auto prev = SelectObject(mem, tmp);
      if (prev && prev != HGDI_ERROR) {
        auto *raw = static_cast<DWORD *>(buf);
        const size_t total = static_cast<size_t>(w) * h;
        std::fill_n(raw, total, 0);
        ok = DrawIconEx(mem, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL) != FALSE;
        if (ok) {
          const bool hasAlpha = std::any_of(raw, raw + total, [](DWORD px) { return (px >> 24) != 0; });
          if (!hasAlpha) {
            std::fill_n(raw, total, 0x00ff00ffu);
            ok = DrawIconEx(mem, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL) != FALSE;
            for (size_t i = 0; ok && i < total; ++i) {
              out[i] = (raw[i] & 0x00ffffffu) == 0x00ff00ffu ? 0u : (0xff000000u | (raw[i] & 0x00ffffffu));
            }
          } else {
            for (size_t i = 0; i < total; ++i) {
              const DWORD px = raw[i];
              const BYTE a = static_cast<BYTE>(px >> 24);
              const BYTE r = static_cast<BYTE>(((px >> 16) & 0xff) * a / 255);
              const BYTE g = static_cast<BYTE>(((px >> 8) & 0xff) * a / 255);
              const BYTE b = static_cast<BYTE>((px & 0xff) * a / 255);
              out[i] = (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(r) << 16) |
                       (static_cast<DWORD>(g) << 8) | static_cast<DWORD>(b);
            }
          }
        }
        SelectObject(mem, prev);
      }
      DeleteDC(mem);
    }
    DeleteObject(tmp);
    return ok;
  };

  std::vector<DWORD> iconPixels;
  if (!drawIconToArgb(icon, iconSize.cx, iconSize.cy, iconPixels)) {
    DeleteObject(bitmap);
    return;
  }

  auto *canvas = static_cast<DWORD *>(pixels);
  std::fill_n(canvas, static_cast<size_t>(canvasW) * canvasH, 0u);
  const auto blendSprite = [&](LONG dstX, LONG dstY, BYTE opacity) {
    for (LONG y = 0; y < iconSize.cy; ++y) {
      const LONG cy = dstY + y;
      if (cy < 0 || cy >= canvasH) continue;
      for (LONG x = 0; x < iconSize.cx; ++x) {
        const LONG cx = dstX + x;
        if (cx < 0 || cx >= canvasW) continue;
        const DWORD src = iconPixels[static_cast<size_t>(y) * iconSize.cx + x];
        const BYTE sa = static_cast<BYTE>((src >> 24) * opacity / 255);
        if (!sa) continue;
        const BYTE sr = static_cast<BYTE>(((src >> 16) & 0xff) * opacity / 255);
        const BYTE sg = static_cast<BYTE>(((src >> 8) & 0xff) * opacity / 255);
        const BYTE sb = static_cast<BYTE>((src & 0xff) * opacity / 255);
        DWORD &dst = canvas[static_cast<size_t>(cy) * canvasW + cx];
        const BYTE da = static_cast<BYTE>(dst >> 24);
        const BYTE dr = static_cast<BYTE>((dst >> 16) & 0xff);
        const BYTE dg = static_cast<BYTE>((dst >> 8) & 0xff);
        const BYTE db = static_cast<BYTE>(dst & 0xff);
        const BYTE inv = static_cast<BYTE>(255 - sa);
        const BYTE outA = static_cast<BYTE>(sa + da * inv / 255);
        const BYTE outR = static_cast<BYTE>(sr + dr * inv / 255);
        const BYTE outG = static_cast<BYTE>(sg + dg * inv / 255);
        const BYTE outB = static_cast<BYTE>(sb + db * inv / 255);
        dst = (static_cast<DWORD>(outA) << 24) | (static_cast<DWORD>(outR) << 16) |
              (static_cast<DWORD>(outG) << 8) | static_cast<DWORD>(outB);
      }
    }
  };

  if (multiStack) {
    blendSprite(iconLeft + stackOffset, std::max(0L, iconTop - stackOffset / 2), 140);
  }
  blendSprite(iconLeft, iconTop, 226);

  if (showLabel || multiStack) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (dc) {
      const auto old = SelectObject(dc, bitmap);
      if (old && old != HGDI_ERROR) {
        Microsoft::WRL::ComPtr<ID2D1Factory> d2d;
        Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d))) && d2d &&
            SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                          reinterpret_cast<IUnknown **>(dwrite.GetAddressOf()))) && dwrite) {
          D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
              D2D1_RENDER_TARGET_TYPE_DEFAULT,
              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
          Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
          RECT bindRect{0, 0, canvasW, canvasH};
          if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &target)) && target &&
              SUCCEEDED(target->BindDC(dc, &bindRect))) {
            target->BeginDraw();
            target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            if (showLabel) {
              const float fontPx = static_cast<float>(std::max(11, MulDiv(12, dpi, 96)));
              Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
              if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                     fontPx, L"", &fmt)) && fmt) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
                if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis)) && ellipsis) {
                  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                  fmt->SetTrimming(&trim, ellipsis.Get());
                }
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> shadowBrush;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush;
                target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.82f), &shadowBrush);
                target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &textBrush);
                const float left = 2.0f;
                const float top = static_cast<float>(iconTop + iconSize.cy + textGap);
                const float right = static_cast<float>(canvasW - 2);
                const float bottom = static_cast<float>(canvasH);
                if (shadowBrush) {
                  target->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), fmt.Get(),
                                    D2D1::RectF(left + 1.0f, top + 1.0f, right + 1.0f, bottom + 1.0f), shadowBrush.Get());
                }
                if (textBrush) {
                  target->DrawTextW(label.c_str(), static_cast<UINT32>(label.size()), fmt.Get(),
                                    D2D1::RectF(left, top, right, bottom), textBrush.Get());
                }
              }
            } else if (multiStack) {
              const auto countText = std::to_wstring(itemCount);
              const float fontPx = static_cast<float>(std::max(11, MulDiv(12, dpi, 96)));
              Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
              if (SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                                     DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                     fontPx, L"", &fmt)) && fmt) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                const float badgeH = static_cast<float>(std::max(18, MulDiv(20, dpi, 96)));
                const float badgeW = std::max(badgeH, badgeH * 0.65f + countText.size() * fontPx * 0.62f);
                const float cx = iconLeft + iconSize.cx * 0.5f;
                const float cy = iconTop + iconSize.cy * 0.5f;
                const D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                    D2D1::RectF(cx - badgeW * 0.5f, cy - badgeH * 0.5f, cx + badgeW * 0.5f, cy + badgeH * 0.5f),
                    4.0f, 4.0f);
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fillBrush;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush;
                target->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.42f, 0.78f, 0.95f), &fillBrush);
                target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &borderBrush);
                if (fillBrush) target->FillRoundedRectangle(pill, fillBrush.Get());
                if (borderBrush) {
                  target->DrawRoundedRectangle(pill, borderBrush.Get(), 1.5f);
                  target->DrawTextW(countText.c_str(), static_cast<UINT32>(countText.size()), fmt.Get(),
                                    pill.rect, borderBrush.Get());
                }
              }
            }
            target->EndDraw();
          }
        }
        SelectObject(dc, old);
      }
      DeleteDC(dc);
    }
  }

  SHDRAGIMAGE image{};
  image.sizeDragImage = {canvasW, canvasH};
  image.ptOffset = {std::clamp(iconLeft + iconAnchor.x, 0L, canvasW - 1),
                    std::clamp(iconTop + iconAnchor.y, 0L, canvasH - 1)};
  image.hbmpDragImage = bitmap;
  image.crColorKey = CLR_NONE;
  if (FAILED(helper->InitializeFromBitmap(&image, data))) DeleteObject(bitmap);
}

void setDropDescription(IDataObject *data, DROPIMAGETYPE type, const wchar_t *message,
                        const wchar_t *insert) {
  if (!data) return;
  static const UINT formatId = RegisterClipboardFormatW(CFSTR_DROPDESCRIPTION);
  if (!formatId) return;
  STGMEDIUM medium{};
  medium.tymed = TYMED_HGLOBAL;
  medium.hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPDESCRIPTION));
  if (!medium.hGlobal) return;
  if (auto *desc = static_cast<DROPDESCRIPTION *>(GlobalLock(medium.hGlobal))) {
    desc->type = type;
    if (message) wcsncpy_s(desc->szMessage, message, _TRUNCATE);
    if (insert) wcsncpy_s(desc->szInsert, insert, _TRUNCATE);
    GlobalUnlock(medium.hGlobal);
    FORMATETC format{static_cast<CLIPFORMAT>(formatId), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (SUCCEEDED(data->SetData(&format, &medium, TRUE))) return;
  }
  GlobalFree(medium.hGlobal);
}

bool itemEditInfo(const std::wstring &path, std::wstring &editText, int &selectLength) {
  editText.clear();
  selectLength = 0;
  if (path.empty()) return false;
  PIDLIST_ABSOLUTE parsed = nullptr;
  if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &parsed, 0, nullptr)) || !parsed) return false;
  const std::unique_ptr<void, decltype(&CoTaskMemFree)> owned(static_cast<void *>(parsed), CoTaskMemFree);
  Microsoft::WRL::ComPtr<IShellFolder> folder;
  PCUITEMID_CHILD child = nullptr;
  if (FAILED(SHBindToParent(parsed, IID_PPV_ARGS(&folder), &child)) || !folder || !child) return false;
  SFGAOF attributes = SFGAO_CANRENAME | SFGAO_FOLDER;
  if (FAILED(folder->GetAttributesOf(1, &child, &attributes)) || !(attributes & SFGAO_CANRENAME)) return false;
  STRRET str{};
  if (FAILED(folder->GetDisplayNameOf(child, SHGDN_FOREDITING | SHGDN_INFOLDER, &str))) return false;
  wchar_t buffer[MAX_PATH * 2]{};
  if (FAILED(StrRetToBufW(&str, child, buffer, ARRAYSIZE(buffer))) || !buffer[0]) return false;
  editText = buffer;
  selectLength = static_cast<int>(editText.size());
  if (!(attributes & SFGAO_FOLDER) && !isDesktopShellItem(path)) {
    const auto extension = fn::fs::path(path).extension().wstring();
    if (!extension.empty() && editText.size() > extension.size() &&
        _wcsicmp(editText.c_str() + editText.size() - extension.size(), extension.c_str()) == 0) {
      selectLength = static_cast<int>(editText.size() - extension.size());
    }
  }
  return true;
}

void DropTarget::clearMarker() {
#ifdef FOLDERNOTE_WINUI
  if (panel.hasSurface()) panel.surface->showItemInsertion(std::nullopt);
#endif
  if (!IsRectEmpty(&panel.desktop.dropMarker)) {
    const RECT previous = panel.desktop.dropMarker;
    panel.desktop.dropMarker = {};
    InvalidateRect(panel.list, &previous, FALSE);
  }
}

void DropTarget::updateDescription(IDataObject *data, DWORD allowed, const std::wstring &folder, bool groupDrop) {
  if (!data) return;
  DROPIMAGETYPE type = DROPIMAGE_NONE;
  std::wstring message;
  std::wstring insert;
  if (allowed) {
    const auto &group = panel.desktop.groups[panel.index];
    const auto targetLabel = [&]() -> std::wstring {
      if (!folder.empty()) {
        if (isRecycleBinShellItem(folder)) return recycleBinDisplayName();
        const auto targetName = fn::fs::path(folder).filename().wstring();
        return targetName.empty() ? folder : targetName;
      }
      if (panel.activeTab > 0 && panel.activeTab <= group.tabs.size() && !group.tabs[panel.activeTab - 1].name.empty()) {
        return group.tabs[panel.activeTab - 1].name;
      }
      return group.name;
    }();
    if (folder.empty() && groupDrop && allowed == DROPEFFECT_MOVE &&
        panel.desktop.oleDragActive && panel.desktop.dragWindow == panel.window) {
      type = DROPIMAGE_NOIMAGE;
    } else if (allowed == DROPEFFECT_MOVE) {
      type = DROPIMAGE_MOVE;
      message = lang::text(L"移动到 %1");
      insert = targetLabel;
    } else if (allowed == DROPEFFECT_LINK) {
      type = DROPIMAGE_LINK;
      message = lang::text(hasShellItems && !files ? L"添加到 %1" : L"在 %1 中创建链接");
      insert = targetLabel;
    } else {
      type = DROPIMAGE_COPY;
      message = lang::text(L"复制到 %1");
      insert = targetLabel;
    }
  }
  if (type == lastDescType && message == lastDescMessage && insert == lastDescInsert) return;
  lastDescType = type;
  lastDescMessage = message;
  lastDescInsert = insert;
  setDropDescription(data, type, message.c_str(), insert.c_str());
}

DWORD DropTarget::applyModifierEffect(DWORD keyState, DWORD allowed, DWORD preferred) {
  constexpr DWORD keyAlt = 0x20;
  const bool ctrl = (keyState & MK_CONTROL) != 0;
  const bool shift = (keyState & MK_SHIFT) != 0;
  const bool alt = (keyState & keyAlt) != 0;
  if ((alt || (ctrl && shift)) && (allowed & DROPEFFECT_LINK)) return DROPEFFECT_LINK;
  if (ctrl && !shift && (allowed & DROPEFFECT_COPY)) return DROPEFFECT_COPY;
  if (shift && !ctrl && (allowed & DROPEFFECT_MOVE)) return DROPEFFECT_MOVE;
  if (allowed & preferred) return preferred;
  if (allowed & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
  if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
  return allowed & DROPEFFECT_LINK;
}

DWORD DropTarget::externalEffect(DWORD keyState, DWORD allowed) const {
  if (panel.folderView() || panel.desktop.editing || panel.desktop.quitting) return DROPEFFECT_NONE;
  if (files && !hasShellItems) {
    return applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
  }
  if (!hasShellItems) return DROPEFFECT_NONE;
  if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
  return allowed & DROPEFFECT_COPY;
}

DWORD DropTarget::evaluate(POINT screen, DWORD keyState, DWORD allowed, std::wstring &folder, bool &groupDrop) const {
  folder.clear();
  groupDrop = false;
  if (panel.desktop.editing || panel.desktop.quitting) return DROPEFFECT_NONE;
  if (panel.desktop.oleDragActive) {
    const auto &paths = panel.desktop.dragItems;
    const bool virtualItems = std::any_of(paths.begin(), paths.end(), isDesktopShellItem);
    auto candidate = !panel.folderView() && virtualItems ? std::wstring{} : panel.desktop.dropFolder(&panel, screen);
    if (candidate.empty() && panel.folderView() && !virtualItems) {
      candidate = panel.activeFolder();
    }
    if (!candidate.empty()) {
      const DWORD chosen = isRecycleBinShellItem(candidate)
          ? (allowed & DROPEFFECT_MOVE)
          : applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
      if (chosen == DROPEFFECT_MOVE ? panel.desktop.canMoveInto(paths, candidate) : (chosen != DROPEFFECT_NONE)) {
        folder = candidate;
        return chosen;
      }
      return DROPEFFECT_NONE;
    }
    if (panel.folderView()) return DROPEFFECT_NONE;
    if (virtualItems) {
      if (!panel.desktop.oleDragFromMapped && (allowed & DROPEFFECT_MOVE)) {
        groupDrop = true;
        return DROPEFFECT_MOVE;
      }
      groupDrop = (allowed & DROPEFFECT_LINK) != 0;
      return allowed & DROPEFFECT_LINK;
    }
    const DWORD chosen = applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
    groupDrop = chosen != DROPEFFECT_NONE;
    return chosen;
  }
  if (files && !hasShellItems) {
    const auto hitTarget = panel.desktop.dropFolder(&panel, screen);
    if (isRecycleBinShellItem(hitTarget) && (allowed & DROPEFFECT_MOVE)) {
      folder = hitTarget;
      return DROPEFFECT_MOVE;
    }
  }
  if (panel.folderView() && files && !hasShellItems) {
    folder = panel.activeFolder();
    return folder.empty() ? DROPEFFECT_NONE : applyModifierEffect(keyState, allowed, DROPEFFECT_MOVE);
  }
  return externalEffect(keyState, allowed);
}

void DropTarget::updateFeedback(POINT screen, DWORD allowed, const std::wstring &folder, bool groupDrop) {
  const auto highlighted = allowed != DROPEFFECT_NONE ? panel.window : nullptr;
  RECT marker{};
  if (groupDrop && folder.empty() && highlighted) {
    panel.autoscroll(screen);
    marker = panel.insertion(screen).marker;
  }
  const bool changedTarget = highlighted != panel.desktop.dropHighlight;
  if (changedTarget) {
    if (panel.desktop.dropHighlight) {
      const auto previous = std::find_if(panel.desktop.panels.begin(), panel.desktop.panels.end(), [&](const auto &item) {
        return item->window == panel.desktop.dropHighlight;
      });
      if (previous != panel.desktop.panels.end()) {
#ifdef FOLDERNOTE_WINUI
        if ((*previous)->hasSurface()) (*previous)->surface->showItemInsertion(std::nullopt);
#endif
        InvalidateRect((*previous)->list, &panel.desktop.dropMarker, FALSE);
      }
    }
    panel.desktop.highlight(highlighted);
  }
#ifdef FOLDERNOTE_WINUI
  if (panel.hasSurface()) {
    panel.surface->showItemInsertion(highlighted && !IsRectEmpty(&marker) ? std::optional<RECT>(marker) : std::nullopt);
  }
#endif
  if (!EqualRect(&marker, &panel.desktop.dropMarker)) {
    if (highlighted && !panel.hasSurface()) {
      InvalidateRect(panel.list, &panel.desktop.dropMarker, FALSE);
      InvalidateRect(panel.list, &marker, FALSE);
    }
    panel.desktop.dropMarker = marker;
  } else if (changedTarget && highlighted && !panel.hasSurface() && !IsRectEmpty(&marker)) {
    InvalidateRect(panel.list, &marker, FALSE);
  }
}

void DropTarget::checkSpringLoadedTab(POINT screen) {
  const auto &group = panel.desktop.groups[panel.index];
  const auto tab = !group.tabs.empty() ? panel.tabAt(screen) : std::nullopt;
  if (tab && (*tab != panel.activeTab || panel.collapsed())) {
    const auto now = GetTickCount64();
    if (dragHoverTab != tab) {
      dragHoverTab = tab;
      dragHoverTabSince = now;
    } else if (now - dragHoverTabSince >= 380) {
      dragHoverTab.reset();
      if (panel.collapsed()) {
        panel.desktop.groups[panel.index].collapsed = false;
        panel.autoCollapsed = false;
        panel.setCollapsed();
      }
      panel.showTab(*tab, true);
    }
  } else {
    dragHoverTab.reset();
  }
}

DropTarget::DropTarget(Panel &value) : panel(value) {
  CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dragImage));
}

HRESULT STDMETHODCALLTYPE DropTarget::QueryInterface(REFIID iid, void **out) {
  *out = nullptr;
  if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
  *out = static_cast<IDropTarget *>(this);
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE DropTarget::AddRef() { return ++references; }

ULONG STDMETHODCALLTYPE DropTarget::Release() {
  const auto remaining = --references;
  if (!remaining) delete this;
  return remaining;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragEnter(IDataObject *data, DWORD keyState, POINTL point, DWORD *effects) {
  DragLeave();
  try {
    if (data) {
      FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      files = data->QueryGetData(&format) == S_OK;
      if (!files || !(*effects & DROPEFFECT_MOVE)) {
        for (const auto &path : desktopShellDropItems(data)) {
          if (isDesktopShellItem(path)) hasShellItems = true;
          else files = true;
        }
      }
    }
  } catch (...) {
    DragLeave();
  }
  currentData = data;
  POINT screen{point.x, point.y};
  checkSpringLoadedTab(screen);
  std::wstring folder;
  bool groupDrop = false;
  *effects = evaluate(screen, keyState, *effects, folder, groupDrop);
  updateFeedback(screen, *effects, folder, groupDrop);
  updateDescription(data, *effects, folder, groupDrop);
  if (dragImage) dragImage->DragEnter(panel.window, data, &screen, *effects);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragOver(DWORD keyState, POINTL point, DWORD *effects) {
  POINT screen{point.x, point.y};
  checkSpringLoadedTab(screen);
  std::wstring folder;
  bool groupDrop = false;
  *effects = evaluate(screen, keyState, *effects, folder, groupDrop);
  updateFeedback(screen, *effects, folder, groupDrop);
  updateDescription(currentData.Get(), *effects, folder, groupDrop);
  if (dragImage) dragImage->DragOver(&screen, *effects);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::DragLeave() {
  files = hasShellItems = false;
  dragHoverTab.reset();
  lastDescType = DROPIMAGE_INVALID;
  lastDescMessage.clear();
  lastDescInsert.clear();
  if (currentData) {
    setDropDescription(currentData.Get(), DROPIMAGE_INVALID);
    currentData.Reset();
  }
  clearMarker();
  panel.desktop.highlight(nullptr);
  if (dragImage) dragImage->DragLeave();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE DropTarget::Drop(IDataObject *data, DWORD keyState, POINTL point, DWORD *effects) {
  const auto offered = *effects;
  POINT screen{point.x, point.y};
  std::wstring folder;
  bool groupDrop = false;
  const auto allowed = evaluate(screen, keyState, offered, folder, groupDrop);
  *effects = DROPEFFECT_NONE;
  files = hasShellItems = false;
  dragHoverTab.reset();
  lastDescType = DROPIMAGE_INVALID;
  lastDescMessage.clear();
  lastDescInsert.clear();
  currentData.Reset();
  setDropDescription(data, DROPIMAGE_INVALID);
  clearMarker();
  panel.desktop.highlight(nullptr);
  DWORD visualEffect = DROPEFFECT_NONE;
  if (allowed) {
    if (panel.desktop.oleDragActive) {
      panel.desktop.pendingInternalDrop = Desktop::InternalDrop{panel.window, std::move(folder), screen, allowed};
      visualEffect = allowed;
      setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
      setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, DROPEFFECT_NONE);
      *effects = DROPEFFECT_NONE;
    } else {
      try {
        auto paths = (allowed == DROPEFFECT_MOVE || files) ? desktopDropPaths(data) : desktopShellDropItems(data);
        if (!folder.empty()) {
          if (panel.folderView() && samePath(folder, panel.activeFolder()) &&
              (allowed == DROPEFFECT_MOVE || allowed == DROPEFFECT_COPY)) {
            const auto ins = panel.insertion(screen);
            size_t slot = ins.item >= 0
                ? static_cast<size_t>(ins.item) + (ins.after ? 1 : 0)
                : panel.orderedItems.size();
            bool anyQueued = false;
            for (const auto &p : paths) {
              if (panel.desktop.enqueueAsyncTransferIntoPanel(panel, p, slot, allowed == DROPEFFECT_MOVE)) {
                ++slot;
                anyQueued = true;
              }
            }
            if (anyQueued) {
              visualEffect = allowed;
              setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
              setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, allowed);
              *effects = allowed == DROPEFFECT_MOVE ? (offered & DROPEFFECT_COPY) : allowed;
            }
          } else {
            panel.desktop.moveInto(paths, folder, panel.window, allowed);
            visualEffect = allowed;
            setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
            setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, allowed);
            *effects = allowed == DROPEFFECT_MOVE ? (offered & DROPEFFECT_COPY) : allowed;
          }
        } else if (allowed == DROPEFFECT_MOVE || (hasShellItems && !files)) {
          if (allowed != DROPEFFECT_MOVE)
            std::erase_if(paths, [](const auto &path) { return !isDesktopShellItem(path); });
          if (panel.desktop.assign(panel, paths, screen, true)) {
            visualEffect = allowed;
            if (allowed == DROPEFFECT_MOVE) {
              // The target moved the files. The drag source must not delete anything.
              setDropEffect(data, CFSTR_PERFORMEDDROPEFFECT, DROPEFFECT_NONE);
              setDropEffect(data, CFSTR_LOGICALPERFORMEDDROPEFFECT, DROPEFFECT_MOVE);
              *effects = offered & DROPEFFECT_COPY;
            } else {
              *effects = allowed;
            }
          }
        } else if (panel.desktop.assignCopyOrLink(panel, paths, screen, allowed)) {
          visualEffect = allowed;
          *effects = allowed;
        }
      } catch (...) {
        PostMessageW(panel.window, rejectedMessage, 0, 0);
      }
    }
  }
  if (dragImage) dragImage->Drop(data, &screen, visualEffect);
  return S_OK;
}

BoxFileMoves::BoxFileMoves(const std::vector<std::wstring> &desktopRoots, HWND window, size_t count)
    : roots(desktopRoots), owner(window) {
  moves.reserve(count);
}

BoxFileMoves::~BoxFileMoves() {
  bool restored = true;
  for (auto move = moves.rbegin(); move != moves.rend(); ++move)
    restored &= rollbackBoxMove(move->second, move->first);
  if (!restored)
    report(owner, lang::text(L"无法恢复原文件位置。请在桌面和 .desktop 文件夹中查看文件。"));
}

const std::wstring &BoxFileMoves::transfer(const std::wstring &path, bool intoBox) {
  auto original = path;
  auto destination = intoBox ? storeBoxItem(roots, original) : restoreBoxItem(roots, original);
  if (destination.empty()) {
    static const std::wstring empty;
    return empty;
  }
  return moves.emplace_back(std::move(original), std::move(destination)).second;
}

void Desktop::clearDropFeedback() {
  if (dropHighlight) {
    const auto previous = std::find_if(panels.begin(), panels.end(), [&](const auto &panel) {
      return panel->window == dropHighlight;
    });
    if (previous != panels.end()) {
#ifdef FOLDERNOTE_WINUI
      if ((*previous)->hasSurface()) (*previous)->surface->showItemInsertion(std::nullopt);
#endif
      InvalidateRect((*previous)->list, &dropMarker, FALSE);
    }
    highlight(nullptr);
  }
  dropMarker = {};
  lastDragScroll = 0;
}

void Desktop::drag(Panel &panel, int index) {
  const auto &items = panel.displayed;
  if (index < 0 || static_cast<size_t>(index) >= items.size() || isTransferToken(items[index]) ||
      editing || dragging || quitting) return;
  const bool fromMapped = panel.folderView();
  const auto resolvePath = [&](const std::wstring &candidate) {
    return fromMapped ? candidate : boxItem(candidate);
  };
  const auto primaryPath = resolvePath(items[index]);
  if (primaryPath.empty()) return;

  struct DragSlotInfo {
    std::wstring displayKey;
    size_t slot = 0;
    bool isDir = false;
  };
  std::vector<std::wstring> paths;
  std::vector<DragSlotInfo> slotInfos;
  const auto recordDragItem = [&](size_t rowIdx) {
    if (isTransferToken(items[rowIdx])) return;
    if (auto resolved = resolvePath(items[rowIdx]); !resolved.empty()) {
      const bool isDir = !isDesktopShellItem(resolved) && PathIsDirectoryW(resolved.c_str()) != FALSE;
      slotInfos.push_back({items[rowIdx], rowIdx, isDir});
      paths.push_back(std::move(resolved));
    }
  };
  if (ListView_GetItemState(panel.list, index, LVIS_SELECTED) & LVIS_SELECTED) {
    for (int row = -1; (row = ListView_GetNextItem(panel.list, row, LVNI_SELECTED)) >= 0;) {
      if (static_cast<size_t>(row) < items.size()) recordDragItem(static_cast<size_t>(row));
    }
  }
  if (paths.empty()) recordDragItem(static_cast<size_t>(index));
  if (paths.empty()) return;

  const auto clearLiftedOnSource = [&]() {
    if (auto *src = panelForWindow(panel.window)) {
      const bool removedAny = std::erase_if(src->activeTransfers, [](const Panel::TransferEntry &e) {
        return e.dragLifted;
      }) != 0;
      if (removedAny) src->syncSurfaceTransfers();
    }
  };

  const HWND sourceWindow = panel.window;
  auto dataObject = createDragDataObject(sourceWindow, paths);
  if (!dataObject) return;

  const bool listMode = groups[panel.index].view == View::List;
  LVITEMW item{};
  item.mask = LVIF_IMAGE;
  item.iItem = index;
  const auto images = ListView_GetImageList(panel.list, listMode ? LVSIL_SMALL : LVSIL_NORMAL);
  HICON icon = nullptr;
  if (!listMode && images && ListView_GetItem(panel.list, &item) && item.iImage >= 0) {
    icon = ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT);
  }
  if (!icon) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(primaryPath.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon) {
      icon = info.hIcon;
    } else if (images && ListView_GetItem(panel.list, &item) && item.iImage >= 0) {
      icon = ImageList_GetIcon(images, item.iImage, ILD_TRANSPARENT);
    }
  }
  if (icon) {
    RECT preview{};
    POINT cursor{};
    bool hasPreview = false;
#ifdef FOLDERNOTE_WINUI
    if (panel.hasSurface()) hasPreview = panel.surface->imageBounds(index, preview);
#endif
    if (!panel.hasSurface() && ListView_GetItemRect(panel.list, index, &preview, LVIR_ICON)) {
      MapWindowPoints(panel.list, HWND_DESKTOP, reinterpret_cast<POINT *>(&preview), 2);
      hasPreview = !IsRectEmpty(&preview);
    }
    const auto dpi = GetDpiForWindow(sourceWindow);
    const LONG minSize = MulDiv(48, dpi, 96);
    SIZE size{minSize, minSize};
    POINT anchor{size.cx / 2, size.cy / 2};
    if (!listMode && hasPreview && GetCursorPos(&cursor)) {
      size = {std::clamp(preview.right - preview.left, minSize, 256L),
              std::clamp(preview.bottom - preview.top, minSize, 256L)};
      anchor = {std::clamp(cursor.x - preview.left, 0L, size.cx - 1),
                std::clamp(cursor.y - preview.top, 0L, size.cy - 1)};
    }
    wchar_t labelBuf[512]{};
    ListView_GetItemText(panel.list, index, 0, labelBuf, ARRAYSIZE(labelBuf));
    std::wstring label = labelBuf[0] ? std::wstring(labelBuf) : fn::fs::path(primaryPath).filename().wstring();
    attachDragImage(dataObject.Get(), icon, size, anchor, label, paths.size(), dpi);
    DestroyIcon(icon);
  }

  DWORD effect = DROPEFFECT_NONE;
  HRESULT dragResult = DRAGDROP_S_CANCEL;
  std::optional<InternalDrop> internalDrop;
  {
    struct DragScope {
      Desktop &owner;
      HWND srcWnd;
      DragScope(Desktop &desktop, HWND source) : owner(desktop), srcWnd(source) {}
      ~DragScope() {
#ifdef FOLDERNOTE_WINUI
        if (auto *srcPanel = owner.panelForWindow(srcWnd); srcPanel && srcPanel->hasSurface()) {
          srcPanel->surface->setDragActive(false);
        }
#endif
        owner.dragging = false;
        owner.oleDragActive = false;
        owner.oleDragFromMapped = false;
        owner.dragWindow = nullptr;
        owner.dragItems.clear();
        owner.clearDropFeedback();
      }
    } scope(*this, sourceWindow);

#ifdef FOLDERNOTE_WINUI
    if (panel.hasSurface()) {
      panel.surface->setDragActive(true);
      for (size_t di = 0; di < paths.size(); ++di) {
        Panel::TransferEntry lifted;
        lifted.id = nextTransferId++;
        lifted.tokenPath = slotInfos[di].displayKey;
        lifted.realPath = paths[di];
        lifted.tab = panel.activeTab;
        lifted.outgoing = true;
        lifted.dragLifted = true;
        lifted.visualProgress = -1.0;
        panel.activeTransfers.push_back(std::move(lifted));
      }
      panel.syncSurfaceTransfers();
    }
#endif
    dragging = true;
    oleDragActive = true;
    oleDragFromMapped = fromMapped;
    dragWindow = sourceWindow;
    dragItems = paths;
    pendingInternalDrop.reset();

    if (GetCapture()) ReleaseCapture();
    if (const HWND desktopRoot = GetAncestor(sourceWindow, GA_ROOT)) {
      SetForegroundWindow(desktopRoot);
    }

    // WinUI 3's InputSiteWindowClass holds a WM_POINTER contact on the UI thread,
    // starving ole32's capture window of legacy WM_MOUSEMOVE / WM_LBUTTONUP messages.
    // Pump live cursor updates directly into the thread's OLE capture HWND, while
    // passing pdsrc=nullptr to SHDoDragDrop so Shell's CDropSource updates DragWindow's
    // dynamic DropDescription (e.g. "移动到 回收站", "固定到 PowerPoint", "移动到 新建盒子").
    const DWORD dragThreadId = GetCurrentThreadId();
    std::atomic<bool> stopPump{false};
    std::thread pump([dragThreadId, &stopPump]() {
      POINT last{LONG_MIN, LONG_MIN};
      bool primed = false;
      const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
      const int primaryButton = swapped ? VK_RBUTTON : VK_LBUTTON;
      const int secondaryButton = swapped ? VK_LBUTTON : VK_RBUTTON;
      while (!stopPump.load(std::memory_order_relaxed)) {
        GUITHREADINFO gui{sizeof(gui)};
        if (GetGUIThreadInfo(dragThreadId, &gui) && gui.hwndCapture) {
          POINT pt{};
          if (GetCursorPos(&pt)) {
            POINT client = pt;
            ScreenToClient(gui.hwndCapture, &client);
            const LPARAM coords = MAKELPARAM(client.x, client.y);
            WPARAM keys = 0;
            if (GetAsyncKeyState(VK_CONTROL) < 0) keys |= MK_CONTROL;
            if (GetAsyncKeyState(VK_SHIFT) < 0) keys |= MK_SHIFT;
            if (GetAsyncKeyState(VK_MENU) < 0) keys |= 0x20; // MK_ALT
            if (GetAsyncKeyState(VK_ESCAPE) < 0) {
              PostMessageW(gui.hwndCapture, WM_KEYDOWN, VK_ESCAPE, 0);
              break;
            }
            if (GetAsyncKeyState(secondaryButton) < 0) {
              PostMessageW(gui.hwndCapture, WM_RBUTTONDOWN, keys | MK_RBUTTON, coords);
              break;
            }
            if (GetAsyncKeyState(primaryButton) < 0) {
              keys |= MK_LBUTTON;
              if (!primed || pt.x != last.x || pt.y != last.y) {
                primed = true;
                last = pt;
                PostMessageW(gui.hwndCapture, WM_MOUSEMOVE, keys, coords);
              }
            } else {
              if (!primed) {
                PostMessageW(gui.hwndCapture, WM_MOUSEMOVE, keys | MK_LBUTTON, coords);
              }
              PostMessageW(gui.hwndCapture, WM_LBUTTONUP, keys, coords);
              break;
            }
          }
        }
        Sleep(8);
      }
    });
    dragResult = SHDoDragDrop(nullptr, dataObject.Get(), nullptr,
                              DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    stopPump.store(true, std::memory_order_relaxed);
    if (pump.joinable()) pump.join();
    internalDrop = std::exchange(pendingInternalDrop, std::nullopt);
  }

  clearLiftedOnSource();
  if (quitting) return;

  const auto startOutgoingVisual = [&](const std::wstring &resolvedCandidate, bool alreadyGone) {
    auto *srcPanel = panelForWindow(sourceWindow);
    if (!srcPanel) return;
    size_t slotHint = srcPanel->orderedItems.size();
    bool isDir = false;
    std::wstring displayKey = resolvedCandidate;
    for (size_t di = 0; di < paths.size(); ++di) {
      if (samePath(paths[di], resolvedCandidate)) {
        slotHint = slotInfos[di].slot;
        isDir = slotInfos[di].isDir;
        displayKey = slotInfos[di].displayKey;
        break;
      }
    }
    if (alreadyGone) {
      addOutgoingFarewell(*srcPanel, resolvedCandidate, slotHint, isDir);
    } else if (!isDesktopShellItem(resolvedCandidate)) {
      std::erase_if(srcPanel->activeTransfers, [&](const Panel::TransferEntry &e) {
        return !e.placeholder && samePath(e.tokenPath, displayKey);
      });
      Panel::TransferEntry out;
      out.id = nextTransferId++;
      out.tokenPath = displayKey;
      out.realPath = resolvedCandidate;
      out.iconSourcePath = resolvedCandidate;
      out.displayLabel = hoverEntryTitle(resolvedCandidate, isDir);
      out.tab = srcPanel->activeTab;
      out.insertSlot = slotHint;
      out.isDirectory = isDir;
      out.outgoing = true;
      out.visualProgress = 0.12;
      out.startedAt = GetTickCount64();
      srcPanel->activeTransfers.push_back(std::move(out));
      srcPanel->syncSurfaceTransfers();
      ensureTransferTimer();
    }
  };

  if (internalDrop) {
    auto *target = panelForWindow(internalDrop->targetPanel);
    if (!internalDrop->folder.empty()) {
      if (internalDrop->effect == DROPEFFECT_MOVE || isRecycleBinShellItem(internalDrop->folder)) {
        for (const auto &p : paths) startOutgoingVisual(p, true);
      }
      moveInto(paths, internalDrop->folder, sourceWindow, internalDrop->effect);
    } else if (target) {
      if (internalDrop->effect == DROPEFFECT_COPY ||
          (internalDrop->effect == DROPEFFECT_LINK &&
           std::any_of(paths.begin(), paths.end(), [](const auto &p) { return !isDesktopShellItem(p); }))) {
        assignCopyOrLink(*target, paths, internalDrop->point, internalDrop->effect);
      } else {
        if (target->window != sourceWindow) {
          for (const auto &p : paths) startOutgoingVisual(p, true);
        }
        assign(*target, paths, internalDrop->point, fromMapped);
      }
    }
    return;
  }

  if (dragResult == DRAGDROP_S_CANCEL) return;

  const auto targetClsid = readTargetClsid(dataObject.Get());
  const bool droppedOnRecycleBin = targetClsid && IsEqualCLSID(*targetClsid, CLSID_RecycleBin);
  const auto logicalPerformed = readDropEffect(dataObject.Get(), CFSTR_LOGICALPERFORMEDDROPEFFECT);
  const auto physicalPerformed = readDropEffect(dataObject.Get(), CFSTR_PERFORMEDDROPEFFECT);
  const DWORD performed = logicalPerformed.value_or(physicalPerformed.value_or(effect));
  const bool isCopyOrLink = !droppedOnRecycleBin && (performed & (DROPEFFECT_COPY | DROPEFFECT_LINK)) != 0;

  const auto missingFile = [](const std::wstring &candidate) {
    return !isDesktopShellItem(candidate) &&
           GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES &&
           GetLastError() == ERROR_FILE_NOT_FOUND;
  };

  if (dragResult == DRAGDROP_S_DROP && !isCopyOrLink) {
    const bool hasRealFiles = std::any_of(paths.begin(), paths.end(), [](const auto &p) { return !isDesktopShellItem(p); });
    if (hasRealFiles) {
      for (int attempt = 0; attempt < 3; ++attempt) {
        if (std::all_of(paths.begin(), paths.end(), [&](const std::wstring &p) {
              return isDesktopShellItem(p) || missingFile(p);
            })) {
          break;
        }
        Sleep(12);
      }
    }
  }

  if (fromMapped) {
    if (std::any_of(paths.begin(), paths.end(), missingFile)) {
      for (const auto &p : paths) {
        if (missingFile(p)) startOutgoingVisual(p, true);
      }
      if (auto *sourcePanel = panelForWindow(sourceWindow)) {
        sourcePanel->mappedResult = {};
        sourcePanel->refresh(groups[sourcePanel->index]);
      }
    } else if (dragResult == DRAGDROP_S_DROP && !isCopyOrLink) {
      for (const auto &p : paths) startOutgoingVisual(p, false);
      lastFolderRefresh = 0;
    }
    return;
  }

  std::vector<std::wstring> movedAway;
  std::vector<std::wstring> remaining;
  for (const auto &candidate : paths) {
    if (missingFile(candidate)) movedAway.push_back(candidate);
    else remaining.push_back(candidate);
  }
  if (!movedAway.empty()) {
    for (const auto &m : movedAway) startOutgoingVisual(m, true);
    auto changed = groups;
    bool removed = false;
    const auto wasMoved = [&](const std::wstring &entry) {
      return std::any_of(movedAway.begin(), movedAway.end(),
                         [&](const std::wstring &moved) { return samePath(entry, moved); });
    };
    for (auto &group : changed) {
      removed |= std::erase_if(group.paths, wasMoved) != 0;
      for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, wasMoved) != 0;
    }
    if (removed) commit(std::move(changed), sourceWindow);
  }

  if (remaining.empty() || isCopyOrLink) return;

  const auto trackPendingMove = [&](const std::wstring &candidate) {
    if (isDesktopShellItem(candidate)) return;
    const bool alreadyTracked = std::any_of(cutPaths.begin(), cutPaths.end(), [&](const std::wstring &p) {
      return samePath(p, candidate);
    });
    if (!alreadyTracked) cutPaths.push_back(candidate);
    startOutgoingVisual(candidate, false);
  };

  if (droppedOnRecycleBin) {
    std::vector<std::wstring> virtualRemaining;
    for (const auto &candidate : remaining) {
      if (isDesktopShellItem(candidate)) virtualRemaining.push_back(candidate);
      else trackPendingMove(candidate);
    }
    if (!virtualRemaining.empty()) ungroup(virtualRemaining, sourceWindow);
    return;
  }

  POINT endPoint{};
  const bool onDesktop = GetCursorPos(&endPoint) && !panelAt(endPoint) && desktopDropAt(endPoint);
  const bool onBlankDesktop = onDesktop && !desktopIconAt(host, endPoint);
  if (onBlankDesktop) {
    std::vector<std::wstring> directUngroup;
    for (const auto &candidate : remaining) {
      if (isDesktopShellItem(candidate)) {
        directUngroup.push_back(candidate);
      } else if (dragResult == DRAGDROP_S_DROP) {
        const auto parent = fn::fs::path(candidate).parent_path().native();
        const bool alreadyOnDesktop = boxStoredItem(roots, candidate).empty() &&
            std::any_of(roots.begin(), roots.end(), [&](const auto &r) { return samePath(parent, r); });
        if (alreadyOnDesktop) directUngroup.push_back(candidate);
        else trackPendingMove(candidate);
      } else {
        directUngroup.push_back(candidate);
      }
    }
    if (!directUngroup.empty()) ungroup(directUngroup, sourceWindow);
  } else if (dragResult == DRAGDROP_S_DROP) {
    for (const auto &candidate : remaining) trackPendingMove(candidate);
  }
}

std::wstring Desktop::dropFolder(Panel *panel, POINT point) const {
  std::wstring path;
  if (panel) {
    if (panel->collapsed()) return {};
    RECT bounds{};
    GetClientRect(panel->window, &bounds);
    bounds.top = panel->headerHeight();
    auto client = point;
    ScreenToClient(panel->window, &client);
    if (!PtInRect(&bounds, client)) return {};

    int row = -1;
#ifdef FOLDERNOTE_WINUI
    if (panel->hasSurface()) row = panel->surface->hit(point);
#endif
    if (!panel->hasSurface() && IsWindowVisible(panel->list)) {
      LVHITTESTINFO item{};
      item.pt = point;
      ScreenToClient(panel->list, &item.pt);
      row = ListView_HitTest(panel->list, &item);
    }
    if (row >= 0 && static_cast<size_t>(row) < panel->displayed.size()) path = panel->displayed[row];
    if (isRecycleBinShellItem(path)) return recycleBinShellPath;
    if (path.empty() || isDesktopShellItem(path) || !PathIsDirectoryW(path.c_str())) {
      return panel->activeFolder();
    }
  }
  return path;
}

bool Desktop::canMoveInto(std::span<const std::wstring> paths, const std::wstring &folder) const {
  if (isRecycleBinShellItem(folder)) {
    return !paths.empty() && std::all_of(paths.begin(), paths.end(), [](const auto &path) {
      return !path.empty() && !isDesktopShellItem(path);
    });
  }
  return !paths.empty() && std::all_of(paths.begin(), paths.end(), [&](const auto &path) {
    if (path.empty() || isDesktopShellItem(path) || samePath(path, folder)) return false;
    if (folder.size() > path.size() && folder[path.size()] == L'\\' &&
        CompareStringOrdinal(path.data(), static_cast<int>(path.size()), folder.data(),
                             static_cast<int>(path.size()), TRUE) == CSTR_EQUAL) return false;
    return !samePath(fn::fs::path(path).parent_path().native(), folder);
  });
}

void Desktop::moveInto(std::span<const std::wstring> paths, const std::wstring &folder, HWND owner, DWORD effect) {
  if (editing || quitting || paths.empty() || folder.empty()) return;
  if (effect == DROPEFFECT_MOVE && !canMoveInto(paths, folder)) return;
  struct TransferScope {
    bool &active;
    explicit TransferScope(bool &flag) : active(flag) { active = true; }
    ~TransferScope() { active = false; }
  } transfer(editing);

  if (effect == DROPEFFECT_LINK && !isRecycleBinShellItem(folder)) {
    bool anyCreated = false;
    for (const auto &path : paths) {
      if (!createShortcutInFolder(path, folder).empty()) anyCreated = true;
    }
    PostMessageW(window, refreshMessage, 0, 0);
    if (!anyCreated && !quitting) report(owner, lang::text(L"无法完成复制 / 移动操作"));
    return;
  }

  using ItemId = std::unique_ptr<void, decltype(&CoTaskMemFree)>;
  std::vector<ItemId> ownedIds;
  std::vector<PCIDLIST_ABSOLUTE> ids;
  ownedIds.reserve(paths.size());
  ids.reserve(paths.size());
  for (const auto &path : paths) {
    PIDLIST_ABSOLUTE id = nullptr;
    HRESULT result = SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr);
    ownedIds.emplace_back(static_cast<void *>(id), CoTaskMemFree);
    if (FAILED(result) || !id) return;
    ids.push_back(id);
  }
  Microsoft::WRL::ComPtr<IShellItemArray> items;
  HRESULT result = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), ids.data(), &items);
  if (SUCCEEDED(result) && isRecycleBinShellItem(folder)) {
    Microsoft::WRL::ComPtr<IFileOperation> op;
    result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op));
    if (SUCCEEDED(result) && op) {
      op->SetOwnerWindow(owner);
      op->SetOperationFlags((GetKeyState(VK_SHIFT) & 0x8000) != 0 ? FOF_WANTNUKEWARNING : FOF_ALLOWUNDO);
      result = op->DeleteItems(items.Get());
      if (SUCCEEDED(result)) {
        SetForegroundWindow(owner);
        result = op->PerformOperations();
        if (result == COPYENGINE_E_USER_CANCELLED) result = S_OK;
      }
    }
  } else if (SUCCEEDED(result)) {
    Microsoft::WRL::ComPtr<IShellItem> destination;
    result = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&destination));
    if (SUCCEEDED(result)) {
      const auto mode = effect == DROPEFFECT_COPY ? quick::MenuMode::Copy : quick::MenuMode::Move;
      result = quick::transfer::perform(owner, items.Get(), destination.Get(), mode, [&] { return !quitting; });
    }
  }

  if (effect == DROPEFFECT_MOVE || isRecycleBinShellItem(folder)) {
    const auto moved = [&](const std::wstring &path) {
      return std::any_of(paths.begin(), paths.end(), [&](const auto &source) {
        return samePath(source, path) && GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND;
      });
    };
    auto changed = groups;
    bool removed = false;
    for (auto &group : changed) {
      removed |= std::erase_if(group.paths, moved) != 0;
      for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, moved) != 0;
    }
    // A cancelled transfer can still have completed some of the requested moves.
    if (removed) commit(std::move(changed), owner);
    else PostMessageW(window, refreshMessage, 0, 0);
  } else {
    PostMessageW(window, refreshMessage, 0, 0);
  }
  if (FAILED(result) && !quitting) report(owner, lang::text(L"无法完成复制 / 移动操作"));
}

bool Desktop::releaseItems(std::vector<Group> changed, std::span<const std::wstring> paths, HWND owner,
                           bool refreshContents) {
  BoxFileMoves moves(roots, owner, paths.size());
  for (const auto &path : paths) {
    if (!boxStoredItem(roots, path).empty() && moves.transfer(path, false).empty()) {
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND)
        continue;
      report(owner, lang::text(L"无法将文件移回桌面。"));
      return false;
    }
  }
  if (!commit(std::move(changed), owner, refreshContents)) return false;
  moves.keep();
  return true;
}

bool Desktop::ungroup(std::span<const std::wstring> paths, HWND owner) {
  if (auto *srcPanel = panelForWindow(owner)) {
    for (const auto &p : paths) {
      auto found = std::find_if(srcPanel->orderedItems.begin(), srcPanel->orderedItems.end(),
                                [&](const auto &item) { return samePath(item, p); });
      const size_t slot = found != srcPanel->orderedItems.end()
                              ? static_cast<size_t>(found - srcPanel->orderedItems.begin())
                              : srcPanel->orderedItems.size();
      const bool isDir = !isDesktopShellItem(p) && PathIsDirectoryW(p.c_str()) != FALSE;
      addOutgoingFarewell(*srcPanel, p, slot, isDir);
    }
  }
  auto changed = groups;
  bool removed = false;
  const auto included = [&](const auto &path) {
    return std::any_of(paths.begin(), paths.end(), [&](const auto &candidate) { return samePath(candidate, path); });
  };
  for (auto &group : changed) {
    removed |= std::erase_if(group.paths, included) != 0;
    for (auto &tab : group.tabs) removed |= std::erase_if(tab.paths, included) != 0;
  }
  return removed && releaseItems(std::move(changed), paths, owner);
}

Insertion Panel::insertion(POINT point) const {
  const auto &group = desktop.groups[index];
  Insertion insertion;
#ifdef FOLDERNOTE_WINUI
  if (surface && surface->available()) {
    if (group.sort != Sort::Manual || collapsed()) return insertion;
    const bool rows = group.view == View::List;
    const auto dpi = GetDpiForWindow(window);
    const auto thickness = std::max(2, MulDiv(3, dpi, 96));
    const auto gap = std::max(2, MulDiv(2, dpi, 96));
    insertion.item = surface->hit(point);
    RECT item{};
    RECT winBounds{};
    GetWindowRect(window, &winBounds);
    RECT contentBounds = winBounds;
    contentBounds.top += headerHeight();
    if (insertion.item >= 0 && surface->itemBounds(insertion.item, item)) {
      insertion.after = rows ? point.y >= (item.top + item.bottom) / 2 :
                               point.x >= (item.left + item.right) / 2;
    } else {
      const int last = static_cast<int>(displayed.size()) - 1;
      if (!PtInRect(&contentBounds, point) || last < 0 || !surface->itemBounds(last, item))
        return insertion;
      insertion.after = true;
    }
    if (rows) {
      const auto edge = insertion.after ? item.bottom : item.top;
      insertion.marker = {item.left + gap * 2, edge - thickness / 2, item.right - gap * 2, edge - thickness / 2 + thickness};
    } else {
      const auto edge = insertion.after ? item.right + gap : item.left - gap;
      insertion.marker = {edge - thickness / 2, item.top + gap * 2, edge - thickness / 2 + thickness, item.bottom - gap * 2};
    }
    IntersectRect(&insertion.marker, &insertion.marker, &contentBounds);
    return insertion;
  }
#endif
  if (group.sort != Sort::Manual || !IsWindowVisible(list)) return insertion;
  LVHITTESTINFO hit{};
  hit.pt = point;
  ScreenToClient(list, &hit.pt);
  insertion.item = ListView_HitTest(list, &hit);
  const bool rows = group.view == View::List;
  const auto thickness = std::max(2, MulDiv(3, GetDpiForWindow(window), 96));
  RECT item{};
  if (insertion.item >= 0 && ListView_GetItemRect(list, insertion.item, &item, rows ? LVIR_BOUNDS : LVIR_ICON)) {
    insertion.after = rows ? hit.pt.y > (item.top + item.bottom) / 2 : hit.pt.x > (item.left + item.right) / 2;
  } else {
    RECT client{};
    GetClientRect(list, &client);
    const auto last = ListView_GetItemCount(list) - 1;
    if (!PtInRect(&client, hit.pt) || last < 0 || !ListView_GetItemRect(list, last, &item, rows ? LVIR_BOUNDS : LVIR_ICON))
      return insertion;
    insertion.after = true;
  }
  if (rows) {
    const auto edge = insertion.after ? item.bottom : item.top;
    insertion.marker = {item.left, edge - thickness / 2, item.right, edge - thickness / 2 + thickness};
  } else {
    const auto edge = insertion.after ? item.right + thickness : item.left - thickness;
    insertion.marker = {edge - thickness / 2, item.top, edge - thickness / 2 + thickness, item.bottom};
  }
  RECT client{};
  GetClientRect(list, &client);
  IntersectRect(&insertion.marker, &insertion.marker, &client);
  return insertion;
}

void Panel::autoscroll(POINT point) {
  if (desktop.groups[index].displayMode == DisplayMode::Pages) return;
#ifdef FOLDERNOTE_WINUI
  if (surface && surface->available()) {
    const auto now = GetTickCount64();
    if (now - desktop.lastDragScroll >= 75 && surface->scrollAt(point)) desktop.lastDragScroll = now;
    return;
  }
#endif
  if (!IsWindowVisible(list)) return;
  const auto now = GetTickCount64();
  if (now - desktop.lastDragScroll < 75) return;
  ScreenToClient(list, &point);
  RECT client{};
  GetClientRect(list, &client);
  const auto dpi = GetDpiForWindow(window);
  const int edge = MulDiv(20, dpi, 96);
  if (point.x < client.left || point.y < client.top || point.x >= client.right || point.y >= client.bottom) return;
  const auto direction = [edge](int position, int extent) {
    return position < std::min(edge, extent / 2) ? -1 : position >= std::max(extent - edge, extent / 2) ? 1 : 0;
  };
  const auto vertical = direction(point.y, client.bottom);
  const auto horizontal = direction(point.x, client.right);
  if (vertical && verticalScroll.maximum())
    verticalScroll.scrollTo(verticalScroll.position() + vertical *
                            (desktop.groups[index].view == View::List ? 1 : MulDiv(22, dpi, 96)));
  if (horizontal && horizontalScroll.maximum())
    horizontalScroll.scrollTo(horizontalScroll.position() + horizontal * MulDiv(22, dpi, 96));
  desktop.lastDragScroll = now;
}

void Desktop::ensureTransferTimer() {
  if (!transferTimerRunning && IsWindow(window)) {
    transferTimerRunning = SetTimer(window, transferTimer, 25, nullptr) != 0;
  }
}

void Desktop::addSettlePulse(Panel &panel, const std::wstring &storedPath) {
  if (storedPath.empty()) return;
  std::erase_if(panel.activeTransfers, [&](const Panel::TransferEntry &e) {
    return samePath(e.tokenPath, storedPath);
  });
  const ULONGLONG now = GetTickCount64();
  Panel::TransferEntry pulse;
  pulse.id = nextTransferId++;
  pulse.tokenPath = storedPath;
  pulse.realPath = storedPath;
  pulse.iconSourcePath = storedPath;
  pulse.tab = panel.activeTab;
  pulse.isDirectory = !isDesktopShellItem(storedPath) && PathIsDirectoryW(storedPath.c_str()) != FALSE;
  pulse.outgoing = false;
  pulse.finished = true;
  pulse.visualProgress = 0.12;
  pulse.startedAt = now;
  pulse.finishedAt = now;
  panel.activeTransfers.push_back(std::move(pulse));
  panel.syncSurfaceTransfers();
  ensureTransferTimer();
}

void Desktop::addOutgoingFarewell(Panel &panel, const std::wstring &removedPath, size_t slotHint, bool isDir) {
  if (removedPath.empty()) return;
  std::erase_if(panel.activeTransfers, [&](const Panel::TransferEntry &e) {
    return samePath(e.tokenPath, removedPath) || samePath(e.realPath, removedPath);
  });
  const ULONGLONG now = GetTickCount64();
  const uint64_t id = nextTransferId++;
  Panel::TransferEntry farewell;
  farewell.id = id;
  farewell.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(id) + L"\\" +
                       fn::fs::path(removedPath).filename().native();
  farewell.realPath = removedPath;
  farewell.iconSourcePath = removedPath;
  farewell.displayLabel = hoverEntryTitle(removedPath, isDir);
  farewell.tab = panel.activeTab;
  farewell.insertSlot = slotHint;
  farewell.isDirectory = isDir;
  farewell.placeholder = true;
  farewell.outgoing = true;
  farewell.finished = true;
  farewell.visualProgress = 0.22;
  farewell.startedAt = now;
  farewell.finishedAt = now;
  panel.activeTransfers.push_back(std::move(farewell));
  ensureTransferTimer();
}

bool Desktop::enqueueAsyncTransferIntoPanel(Panel &panel, const std::wstring &sourcePath, size_t insertSlot, bool isMove) {
  if (quitting || sourcePath.empty() || isDesktopShellItem(sourcePath)) return false;
  const DWORD attrs = GetFileAttributesW(sourcePath.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES) return false;
  const bool intoMapped = panel.folderView();
  const std::wstring targetFolder = intoMapped ? panel.activeFolder() : boxStorageFolder(roots);
  if (targetFolder.empty()) return false;
  if (samePath(sourcePath, targetFolder) || samePath(sourcePath, boxStorageFolder(roots)) ||
      std::any_of(roots.begin(), roots.end(), [&](const auto &r) { return samePath(sourcePath, r); })) {
    return false;
  }
  if (intoMapped && isMove && samePath(fn::fs::path(sourcePath).parent_path().native(), targetFolder)) {
    return false;
  }

  const bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const uint64_t id = nextTransferId++;
  auto progress = std::make_shared<std::atomic<double>>(0.0);

  Panel::TransferEntry entry;
  entry.id = id;
  entry.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(id) + L"\\" +
                    fn::fs::path(sourcePath).filename().native();
  entry.realPath = sourcePath;
  entry.iconSourcePath = sourcePath;
  entry.displayLabel = hoverEntryTitle(sourcePath, isDir);
  entry.tab = panel.activeTab;
  entry.insertSlot = insertSlot;
  entry.isDirectory = isDir;
  entry.placeholder = true;
  entry.outgoing = false;
  entry.finished = false;
  entry.visualProgress = 0.06;
  entry.sharedProgress = progress;
  entry.startedAt = GetTickCount64();
  panel.activeTransfers.push_back(std::move(entry));
  panel.refresh(groups[panel.index]);
  ensureTransferTimer();

  const HWND desktopWnd = window;
  const HWND panelWnd = panel.window;
  const auto rootsCopy = roots;
  const auto queue = completionQueue;
  std::thread([id, desktopWnd, panelWnd, sourcePath, targetFolder, rootsCopy, isMove, intoMapped, progress, queue] {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring resultPath;
    if (SUCCEEDED(com) || com == RPC_E_CHANGED_MODE) {
      const auto reportRatio = [progress](double r) {
        double prev = progress->load(std::memory_order_relaxed);
        if (r > prev) progress->store(r, std::memory_order_relaxed);
      };
      if (intoMapped) {
        resultPath = transferItemToFolder(sourcePath, targetFolder, isMove, panelWnd, false, reportRatio);
      } else if (isMove) {
        resultPath = moveToBoxStorage(rootsCopy, sourcePath, panelWnd, reportRatio);
      } else {
        resultPath = copyToBoxStorage(rootsCopy, sourcePath, panelWnd, reportRatio);
      }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    {
      std::lock_guard lock(queue->mutex);
      queue->items.push_back({id, panelWnd, sourcePath, std::move(resultPath), isMove, intoMapped});
    }
    if (IsWindow(desktopWnd)) PostMessageW(desktopWnd, transferDoneMessage, 0, 0);
  }).detach();
  return true;
}

void Desktop::handleCompletedTransfers() {
  if (quitting) return;
  std::vector<CompletedTransfer> doneList;
  {
    std::lock_guard lock(completionQueue->mutex);
    doneList.swap(completionQueue->items);
  }
  if (doneList.empty()) return;

  const ULONGLONG now = GetTickCount64();
  for (auto &done : doneList) {
    auto *panel = panelForWindow(done.panelWindow);
    if (!panel) continue;
    auto entryIt = std::find_if(panel->activeTransfers.begin(), panel->activeTransfers.end(),
                                [&](const Panel::TransferEntry &e) { return e.id == done.id; });
    if (done.resultPath.empty()) {
      if (entryIt != panel->activeTransfers.end()) panel->activeTransfers.erase(entryIt);
      panel->refresh(groups[panel->index]);
      continue;
    }

    const size_t desiredTab = entryIt != panel->activeTransfers.end() ? entryIt->tab : panel->activeTab;
    const size_t desiredSlot = entryIt != panel->activeTransfers.end() ? entryIt->insertSlot : SIZE_MAX;

    if (done.intoMapped) {
      WIN32_FILE_ATTRIBUTE_DATA data{};
      ULONGLONG mod = 0;
      DWORD attr = FILE_ATTRIBUTE_NORMAL;
      if (GetFileAttributesExW(done.resultPath.c_str(), GetFileExInfoStandard, &data)) {
        mod = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        attr = data.dwFileAttributes;
      }
      if (std::none_of(panel->mappedItems.begin(), panel->mappedItems.end(),
                       [&](const Panel::MappedItem &m) { return samePath(m.path, done.resultPath); })) {
        panel->mappedItems.push_back({done.resultPath, mod, attr});
      }
      panel->mappedResult = {};
      if (done.isMove && GetFileAttributesW(done.sourcePath.c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetLastError() == ERROR_FILE_NOT_FOUND) {
        auto changed = groups;
        bool removed = false;
        const auto matchSrc = [&](const std::wstring &p) { return samePath(p, done.sourcePath); };
        for (auto &g : changed) {
          removed |= std::erase_if(g.paths, matchSrc) != 0;
          for (auto &t : g.tabs) removed |= std::erase_if(t.paths, matchSrc) != 0;
        }
        if (removed) commit(std::move(changed), panel->window);
      }
    } else {
      auto changed = groups;
      if (done.isMove) {
        const auto removeOld = [&](std::vector<std::wstring> &entries) {
          std::erase_if(entries, [&](const auto &p) {
            return samePath(p, done.sourcePath) && !samePath(p, done.resultPath);
          });
        };
        for (auto &g : changed) {
          removeOld(g.paths);
          for (auto &t : g.tabs) removeOld(t.paths);
        }
      }
      addItem(changed, panel->index, done.resultPath, desiredTab);
      if (changed[panel->index].sort == Sort::Manual) {
        auto &tabItems = manualItems(changed[panel->index], desiredTab);
        auto found = std::find_if(tabItems.begin(), tabItems.end(),
                                  [&](const auto &p) { return samePath(p, done.resultPath); });
        if (found != tabItems.end() && desiredSlot < tabItems.size()) {
          const size_t from = static_cast<size_t>(found - tabItems.begin());
          const size_t to = std::min(desiredSlot, tabItems.size() - 1);
          if (from < to) std::rotate(tabItems.begin() + from, tabItems.begin() + from + 1, tabItems.begin() + to + 1);
          else if (to < from) std::rotate(tabItems.begin() + to, tabItems.begin() + from, tabItems.begin() + from + 1);
        }
      }
      commit(std::move(changed), panel->window, false);
    }

    if (entryIt != panel->activeTransfers.end()) {
      entryIt->tokenPath = done.resultPath;
      entryIt->realPath = done.resultPath;
      entryIt->iconSourcePath = done.resultPath;
      entryIt->placeholder = false;
      entryIt->finished = true;
      entryIt->finishedAt = now;
      entryIt->visualProgress = std::max(entryIt->visualProgress, 0.72);
    }
    for (size_t pi = 0; pi < panels.size(); ++pi) panels[pi]->refresh(groups[pi]);
  }
  ensureTransferTimer();
}

void Desktop::tickTransfers() {
  if (quitting) return;
  const ULONGLONG now = GetTickCount64();
  bool anyActive = false;
  bool anyMissingCut = false;
  const bool shellDialogActive = FindWindowW(L"OperationStatusWindow", nullptr) != nullptr ||
                                 FindWindowW(L"#32770", nullptr) != nullptr;

  for (const auto &panel : panels) {
    if (panel->activeTransfers.empty()) continue;
    bool changedVisual = false;
    bool needsRefresh = false;

    for (size_t i = 0; i < panel->activeTransfers.size();) {
      auto &entry = panel->activeTransfers[i];
      if (entry.dragLifted) {
        anyActive = true;
        ++i;
        continue;
      }
      if (entry.finished) {
        const double next = std::min(1.0, entry.visualProgress + std::max(0.11, (1.0 - entry.visualProgress) * 0.40));
        if (std::abs(next - entry.visualProgress) > 0.002) {
          entry.visualProgress = next;
          changedVisual = true;
        }
        if (entry.visualProgress >= 0.995 && now - entry.finishedAt >= 90) {
          const bool wasPlaceholder = entry.placeholder;
          panel->activeTransfers.erase(panel->activeTransfers.begin() + static_cast<ptrdiff_t>(i));
          if (wasPlaceholder) needsRefresh = true;
          changedVisual = true;
          continue;
        }
        anyActive = true;
        ++i;
        continue;
      }

      if (!entry.outgoing) {
        const double real = entry.sharedProgress ? entry.sharedProgress->load(std::memory_order_relaxed) : 0.0;
        const double target = real > 0.01
            ? std::clamp(0.08 + real * 0.88, entry.visualProgress, 0.96)
            : std::min(0.26, 0.06 + static_cast<double>(now - entry.startedAt) / 5500.0);
        if (target > entry.visualProgress) {
          entry.visualProgress = std::min(target, entry.visualProgress + std::max(0.008, (target - entry.visualProgress) * 0.28));
          changedVisual = true;
        }
        anyActive = true;
        ++i;
        continue;
      }

      // Active outgoing transfer (async Shell move / Recycle Bin)
      const bool fileGone = !entry.realPath.empty() &&
          GetFileAttributesW(entry.realPath.c_str()) == INVALID_FILE_ATTRIBUTES &&
          GetLastError() == ERROR_FILE_NOT_FOUND;
      if (fileGone) {
        if (panel->folderView()) {
          entry.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(entry.id) + L"\\" +
                            fn::fs::path(entry.realPath).filename().native();
          entry.placeholder = true;
          panel->mappedResult = {};
          needsRefresh = true;
        } else {
          const auto realTarget = entry.realPath;
          auto found = std::find_if(panel->orderedItems.begin(), panel->orderedItems.end(),
                                    [&](const auto &p) { return samePath(p, entry.tokenPath); });
          if (found != panel->orderedItems.end()) {
            entry.insertSlot = static_cast<size_t>(found - panel->orderedItems.begin());
          }
          entry.tokenPath = std::wstring(transferTokenPrefix) + std::to_wstring(entry.id) + L"\\" +
                            fn::fs::path(realTarget).filename().native();
          entry.placeholder = true;
          anyMissingCut = true;
          needsRefresh = true;
        }
        entry.finished = true;
        entry.finishedAt = now;
        entry.visualProgress = std::max(entry.visualProgress, 0.68);
        changedVisual = true;
        anyActive = true;
        ++i;
        continue;
      }

      if (now - entry.startedAt > 1800 && !shellDialogActive) {
        panel->activeTransfers.erase(panel->activeTransfers.begin() + static_cast<ptrdiff_t>(i));
        changedVisual = true;
        continue;
      }

      const double target = std::min(0.88, 0.12 + static_cast<double>(now - entry.startedAt) / 7000.0);
      if (target > entry.visualProgress) {
        entry.visualProgress = std::min(target, entry.visualProgress + 0.015);
        changedVisual = true;
      }
      anyActive = true;
      ++i;
    }

    if (needsRefresh && !anyMissingCut) {
      panel->refresh(groups[panel->index]);
    } else if (changedVisual) {
      panel->syncSurfaceTransfers();
    }
  }

  if (anyMissingCut) cleanupMovedCutItems();

  if (!anyActive && transferTimerRunning) {
    KillTimer(window, transferTimer);
    transferTimerRunning = false;
  }
}

}  // namespace desktop_groups::detail
