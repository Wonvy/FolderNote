#include "../src/core.h"
#include "../src/desktop_groups_icon_source.h"
#include "../src/desktop_groups_internal.h"
#include "../src/desktop_groups_model.h"
#include "../src/desktop_groups_render.h"
#include "../src/desktop_groups_storage.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <shlobj.h>
#include <stdexcept>
#include <string_view>
#include <wrl/implements.h>

namespace {
constexpr wchar_t settings[] = L"Software\\ShibaKit";
constexpr wchar_t value[] = L"DesktopGroups";

void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void exerciseShadow() {
  using namespace desktop_groups;
  using Region = std::unique_ptr<std::remove_pointer_t<HRGN>, decltype(&DeleteObject)>;
  constexpr int boxWidth = 71, boxHeight = 30, spread = 12;
  constexpr int width = boxWidth + 2 * spread, height = boxHeight + 2 * spread;
  std::vector<DWORD> pixels(width * height);
  for (const int diameter : {0, 19, 60}) {
    const Region box(createBoxRegion(boxWidth, boxHeight, diameter), DeleteObject);
    check(box != nullptr, "create the desktop box region");
    check(PtInRegion(box.get(), boxWidth - 1, boxHeight / 2) &&
          PtInRegion(box.get(), boxWidth / 2, boxHeight - 1) &&
          !PtInRegion(box.get(), boxWidth, boxHeight / 2) &&
          !PtInRegion(box.get(), boxWidth / 2, boxHeight),
          "keep the final row and column inside the window bounds");
    for (const int offset : {0, 3, 20}) {
      check(drawBoxShadow(pixels.data(), width, height, spread, offset, diameter, 180),
            "render the shadow at different offsets");
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          if (PtInRegion(box.get(), x - spread, y - spread + offset))
            check(pixels[y * width + x] == 0, "shadow must leave the box interior transparent");
        }
      }
      const int middle = spread + boxWidth / 2;
      const int bottom = spread + boxHeight;
      check(pixels[bottom * width + middle] > pixels[(height - 1) * width + middle],
            "shadow remains visible outside the box and fades away from its edge");
    }
  }
}

void exerciseThumbnailMask() {
  using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)>;
  using Icon = std::unique_ptr<std::remove_pointer_t<HICON>, decltype(&DestroyIcon)>;
  using Images = std::unique_ptr<std::remove_pointer_t<HIMAGELIST>, decltype(&ImageList_Destroy)>;
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
  check(InitCommonControlsEx(&controls), "initialize real image lists");
  for (const int size : {24, 32, 48, 64, 96, 144, 192}) {
    BITMAPINFO format{};
    format.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
    void *rawPixels = nullptr;
    const Bitmap bitmap(CreateDIBSection(nullptr, &format, DIB_RGB_COLORS, &rawPixels, nullptr, 0), DeleteObject);
    check(bitmap && rawPixels, "create thumbnail pixels");
    auto pixels = static_cast<DWORD *>(rawPixels);
    const int glyphSize = std::max(1, size / 5);
    const int stride = ((size + 15) / 16) * 2;
    std::vector<BYTE> maskBits(static_cast<size_t>(stride) * size, 0xff);
    std::fill_n(pixels, size * size, 0);
    for (int y = 0; y < glyphSize; ++y) for (int x = 0; x < glyphSize; ++x) {
      pixels[y * size + x] = 0xff7090b0;
      maskBits[y * stride + x / 8] &= static_cast<BYTE>(~(0x80 >> (x % 8)));
    }
    const Bitmap mask(CreateBitmap(size, size, 1, 1, maskBits.data()), DeleteObject);
    ICONINFO info{TRUE, 0, 0, mask.get(), bitmap.get()};
    const Icon placeholder(CreateIconIndirect(&info), DestroyIcon);
    const Images images(ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 0, 2), ImageList_Destroy);
    check(mask && placeholder && images && ImageList_AddIcon(images.get(), placeholder.get()) == 0,
          "seed a small placeholder on a large icon canvas");
    const auto readPixels = [&](int index) {
      const Icon icon(ImageList_GetIcon(images.get(), index, ILD_TRANSPARENT), DestroyIcon);
      ICONINFO extracted{};
      check(icon && GetIconInfo(icon.get(), &extracted), "extract the displayed thumbnail");
      const Bitmap color(extracted.hbmColor, DeleteObject), iconMask(extracted.hbmMask, DeleteObject);
      std::vector<DWORD> result(static_cast<size_t>(size) * size);
      const auto dc = CreateCompatibleDC(nullptr);
      const auto rows = GetDIBits(dc, color.get(), 0, size, result.data(), &format, DIB_RGB_COLORS);
      DeleteDC(dc);
      check(rows == size, "read displayed thumbnail alpha");
      return result;
    };
    std::fill_n(pixels, size * size, 0xff30a0d0);
    check(desktop_groups::setDesktopThumbnail(images.get(), 0, bitmap.get()) == 0,
          "replace placeholder with an opaque photograph");
    const auto photo = readPixels(0);
    check(std::all_of(photo.begin(), photo.end(), [](DWORD pixel) { return pixel >> 24 == 255; }),
          "a full thumbnail must not retain the tiny placeholder mask after zoom");
    check(desktop_groups::setDesktopThumbnail(images.get(), -1, bitmap.get()) == 1 && readPixels(1) == photo,
          "cached and newly loaded thumbnails have identical pixels");
    std::fill_n(pixels, size * size, 0);
    for (int y = 0; y < size; ++y) for (int x = size / 4; x < size * 3 / 4; ++x)
      pixels[y * size + x] = 0xff30a0d0;
    check(desktop_groups::setDesktopThumbnail(images.get(), 0, bitmap.get()) == 0,
          "replace with an alpha thumbnail");
    const auto transparent = readPixels(0);
    check(transparent.front() == 0 && transparent[size / 2] == 0xff30a0d0 && transparent.back() == 0,
          "resetting the old mask preserves the new thumbnail's transparency");
  }
}

struct TemporaryFiles {
  fn::fs::path root = fn::fs::temp_directory_path() /
      (L"ShibaKitDesktopGroups-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));

  TemporaryFiles() { check(fn::fs::create_directory(root), "create temporary files directory"); }
  ~TemporaryFiles() {
    std::error_code error;
    fn::fs::remove_all(root, error);
  }
};

void exerciseThumbnailFit(const fn::fs::path &root) {
  struct ComSession {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComSession() { if (SUCCEEDED(status)) CoUninitialize(); }
  } com;
  check(SUCCEEDED(com.status), "initialize Shell thumbnail extraction");
  using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)>;
  const DWORD corners[]{0xffd03020, 0xff20c040, 0xff3050d0, 0xffd0b020};
  for (const SIZE shape : {SIZE{160, 80}, SIZE{80, 160}, SIZE{480, 24}, SIZE{24, 480}, SIZE{80, 80}, SIZE{9, 5}}) {
    std::vector<DWORD> source(static_cast<size_t>(shape.cx) * shape.cy, 0xff808080);
    for (int y = 0; y < shape.cy; ++y) for (int x = 0; x < shape.cx; ++x) {
      if ((x < shape.cx / 3 || x >= shape.cx * 2 / 3) && (y < shape.cy / 3 || y >= shape.cy * 2 / 3))
        source[y * shape.cx + x] = corners[(y >= shape.cy / 2 ? 2 : 0) + (x >= shape.cx / 2 ? 1 : 0)];
    }
    BITMAPINFOHEADER sourceFormat{sizeof(BITMAPINFOHEADER), shape.cx, -shape.cy, 1, 32, BI_RGB};
    BITMAPFILEHEADER fileHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(sourceFormat);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(source.size() * sizeof(DWORD));
    const auto path = root / (L"thumbnail-" + std::to_wstring(shape.cx) + L"x" + std::to_wstring(shape.cy) + L".bmp");
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(&fileHeader), sizeof(fileHeader));
    file.write(reinterpret_cast<const char *>(&sourceFormat), sizeof(sourceFormat));
    file.write(reinterpret_cast<const char *>(source.data()), source.size() * sizeof(DWORD));
    file.close();
    check(static_cast<bool>(file), "write a photograph with distinct corner markers");
    for (const int size : {24, 32, 48, 64, 96, 144, 192}) {
      const Bitmap image(desktop_groups::loadDesktopThumbnail(path.native(), size), DeleteObject);
      BITMAP dimensions{};
      check(image && GetObjectW(image.get(), sizeof(dimensions), &dimensions) &&
            dimensions.bmWidth == size && dimensions.bmHeight == size,
            "aspect-fitted thumbnails retain the square image-list canvas at every zoom");
      BITMAPINFO format{};
      format.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB};
      std::vector<DWORD> pixels(static_cast<size_t>(size) * size);
      const auto dc = CreateCompatibleDC(nullptr);
      const auto rows = GetDIBits(dc, image.get(), 0, size, pixels.data(), &format, DIB_RGB_COLORS);
      DeleteDC(dc);
      check(rows == size, "read the complete fitted thumbnail");
      RECT content{size, size, 0, 0};
      for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        if (pixels[y * size + x] >> 24) {
          content.left = std::min(content.left, static_cast<LONG>(x));
          content.top = std::min(content.top, static_cast<LONG>(y));
          content.right = std::max(content.right, static_cast<LONG>(x + 1));
          content.bottom = std::max(content.bottom, static_cast<LONG>(y + 1));
        } else check(pixels[y * size + x] == 0, "padding stays fully transparent");
      }
      const auto width = content.right - content.left, height = content.bottom - content.top;
      const auto longest = std::max(shape.cx, shape.cy);
      check(std::max(width, height) == size &&
            std::abs(width - MulDiv(shape.cx, size, longest)) <= 1 &&
            std::abs(height - MulDiv(shape.cy, size, longest)) <= 1,
            "landscape, portrait and panoramic pictures keep their original aspect ratio");
      check(std::abs(content.left - (size - content.right)) <= 1 &&
            std::abs(content.top - (size - content.bottom)) <= 1,
            "uncropped pictures stay centered in the icon area");
      // Very thin or tiny sources necessarily blend the short-axis markers.
      if (std::min(width, height) < 6 || std::min(shape.cx, shape.cy) < 6) continue;
      for (int corner = 0; corner < 4; ++corner) {
        const auto x = content.left + (corner % 2 ? width - 1 - width / 8 : width / 8);
        const auto y = content.top + (corner / 2 ? height - 1 - height / 8 : height / 8);
        const auto actual = pixels[y * size + x], expected = corners[corner];
        for (const int shift : {0, 8, 16}) {
          if (std::abs(static_cast<int>((actual >> shift) & 255) - static_cast<int>((expected >> shift) & 255)) >= 40)
            throw std::runtime_error("thumbnail corner lost: " + std::to_string(shape.cx) + "x" +
                std::to_string(shape.cy) + " at " + std::to_string(size) + ", corner " + std::to_string(corner) +
                ", actual " + std::to_string(actual) + ", expected " + std::to_string(expected));
        }
      }
    }
  }
}

class ShellIdListDrop final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject> {
  Microsoft::WRL::ComPtr<IDataObject> source;
  FORMATETC idList{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)),
                   nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};

public:
  explicit ShellIdListDrop(IDataObject *data) : source(data) {}

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *content) override {
    return format && format->cfFormat == idList.cfFormat ? source->GetData(format, content) : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override {
    return format && format->cfFormat == idList.cfFormat ? source->QueryGetData(format) : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **formats) override {
    return direction == DATADIR_GET ? SHCreateStdEnumFmtEtc(1, &idList, formats) : E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }
};

void exerciseShellIdListDrop(const fn::fs::path &root) {
  struct OleSession {
    HRESULT status = OleInitialize(nullptr);
    ~OleSession() { if (SUCCEEDED(status)) OleUninitialize(); }
  } ole;
  check(SUCCEEDED(ole.status), "initialize OLE for the Shell drop fixture");
  const auto folder = root / L"Project folder";
  const auto file = root / L"Notes.txt";
  check(fn::fs::create_directories(folder), "create the Shell folder fixture");
  check(static_cast<bool>(std::ofstream(file) << "contents"), "create the Shell file fixture");

  struct FreePidl {
    using pointer = PIDLIST_ABSOLUTE;
    void operator()(pointer id) const { CoTaskMemFree(id); }
  };
  const auto parse = [](const fn::fs::path &path) {
    PIDLIST_ABSOLUTE id = nullptr;
    check(SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr)) && id,
          "parse the fixture using the Windows Shell");
    return std::unique_ptr<ITEMIDLIST, FreePidl>(id);
  };
  const auto parentId = parse(root), folderId = parse(folder), fileId = parse(file);
  PCUITEMID_CHILD children[]{ILFindLastID(folderId.get()), ILFindLastID(fileId.get())};
  Microsoft::WRL::ComPtr<IDataObject> nativeDrop;
  check(SUCCEEDED(SHCreateDataObject(parentId.get(), ARRAYSIZE(children), children, nullptr, IID_PPV_ARGS(&nativeDrop))) &&
        nativeDrop, "create a real Shell drop containing a folder and a file");
  auto drop = Microsoft::WRL::Make<ShellIdListDrop>(nativeDrop.Get());
  FORMATETC pathsFormat{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  check(drop && drop->QueryGetData(&pathsFormat) == DV_E_FORMATETC, "the fixture offers no CF_HDROP fallback");

  const auto paths = desktop_groups::desktopShellDropItems(drop.Get());
  check(paths.size() == 2 && fn::fs::equivalent(paths[0], folder) && fn::fs::equivalent(paths[1], file),
        "recover a folder and a file using only the Shell ID list");
}

void exerciseBoxStorage(const fn::fs::path &root) {
  using namespace desktop_groups;
  const auto personal = root / L"Personal Desktop";
  const auto shared = root / L"Public Desktop";
  const auto nested = personal / L"Folder" / L"nested.txt";
  const auto outside = root / L"outside.txt";
  const auto document = personal / L"Report.txt";
  const auto duplicate = shared / document.filename();
  fn::fs::create_directories(nested.parent_path());
  fn::fs::create_directory(shared);
  for (const auto &file : {outside, document, duplicate, nested}) std::ofstream(file) << "contents";

  const std::vector<std::wstring> roots{personal.native(), shared.native()};
  const fn::fs::path folder(boxStorageFolder(roots));
  check(folder.parent_path() == personal && folder.filename() == L".desktop" && !fn::fs::exists(folder),
        "choose a lazy personal Desktop .desktop folder");
  check(storeBoxItem(roots, outside.native()).empty() && storeBoxItem(roots, nested.native()).empty() &&
        storeBoxItem(roots, personal.native()).empty() && !fn::fs::exists(folder),
        "reject paths outside the direct Desktop and leave it unchanged");

  const auto moved = storeBoxItem(roots, (personal / L"REPORT.txt").native());
  check(moved == (folder / document.filename()).native() && fn::fs::exists(moved) && !fn::fs::exists(document) &&
        boxStoredItem(roots, moved) == moved &&
        (GetFileAttributesW(folder.c_str()) & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) ==
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM),
        "move the actual document into a hidden system .desktop folder and preserve the original name");
  check(storeBoxItem(roots, folder.native()).empty() && storeBoxItem(roots, moved).empty() &&
        boxStoredItem(roots, folder.native()).empty(), "never organize the storage folder or move a stored item again");

  const auto distinct = storeBoxItem(roots, duplicate.native());
  check(distinct == (folder / L"Report (2).txt").native() &&
        fn::fs::exists(distinct) && fn::fs::exists(moved) && !fn::fs::exists(duplicate),
        "give same-name Desktop items different paths without overwriting");
  check(rollbackBoxMove(distinct, duplicate.native()) && !fn::fs::exists(distinct) && fn::fs::exists(duplicate),
        "roll back a move to its exact source after a layout failure");

  std::ofstream(document) << "new content";
  check(!rollbackBoxMove(moved, document.native()) && fn::fs::exists(moved), "a rollback also preserves collisions");
  const auto restored = restoreBoxItem(roots, moved);
  check(restored == (personal / L"Report (2).txt").native() && fn::fs::exists(restored) &&
        !fn::fs::exists(moved) && fn::fs::exists(document), "return to the Desktop without overwriting a new file");

  const auto movedFolder = storeBoxItem(roots, nested.parent_path().native());
  check(movedFolder == (folder / nested.parent_path().filename()).native() &&
        fn::fs::exists(fn::fs::path(movedFolder) / nested.filename()) && !fn::fs::exists(nested.parent_path()) &&
        restoreBoxItem(roots, movedFolder) == nested.parent_path().native() && fn::fs::exists(nested),
        "round-trip a directory with its contents intact");

  check(fn::fs::remove(folder), "remove the empty test storage directory");
  std::error_code linkError;
  fn::fs::create_directory_symlink(nested.parent_path(), folder, linkError);
  if (!linkError) {
    check(boxStoredItem(roots, (folder / nested.filename()).native()).empty() &&
          storeBoxItem(roots, document.native()).empty() && fn::fs::exists(document),
          "reject a storage path redirected through a directory link");
    check(fn::fs::remove(folder), "remove the test directory link");
  }
  std::ofstream(folder) << "existing file at the reserved path";
  check(storeBoxItem(roots, document.native()).empty() && fn::fs::is_regular_file(folder) &&
        fn::fs::exists(document), "leave the file alone when the storage path is unavailable");
}

struct TemporaryRegistry {
  HKEY user = nullptr, key = nullptr;
  std::wstring path;
  bool redirected = false;

  ~TemporaryRegistry() {
    if (redirected) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (key) {
      RegCloseKey(key);
      RegDeleteTreeW(user, path.c_str());
    }
    if (user) RegCloseKey(user);
  }

  void redirect(const std::wstring &name) {
    path = L"Software\\" + name;
    DWORD disposition = 0;
    check(RegOpenCurrentUser(KEY_ALL_ACCESS, &user) == ERROR_SUCCESS, "open test registry");
    HKEY temporary = nullptr;
    const auto result = RegCreateKeyExW(user, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                                        &temporary, &disposition);
    check(result == ERROR_SUCCESS, "create test registry");
    if (disposition != REG_CREATED_NEW_KEY) {
      RegCloseKey(temporary);
      throw std::runtime_error("test registry already exists");
    }
    key = temporary;
    check(RegOverridePredefKey(HKEY_CURRENT_USER, key) == ERROR_SUCCESS, "redirect test registry");
    redirected = true;
  }
};

void exercise(const fn::fs::path &root) {
  using namespace desktop_groups;
  const auto layout = root / L"settings" / L"DesktopGroups.json";
  const auto readFile = [](const fn::fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
  const auto personal = root / L"Desktop";
  const auto shared = root / L"Public";
  const auto folder = personal / L"文件夹";
  fn::fs::create_directories(folder);
  fn::fs::create_directory(shared);
  const auto document = personal / L"简报.txt";
  const auto sharedDocument = shared / L"Public.txt";
  const auto systemFile = personal / L"desktop.ini";
  const auto child = folder / L"nested.txt";
  const auto outside = root / L"outside.txt";
  for (const auto &path : {document, sharedDocument, systemFile, child, outside})
    std::ofstream(path) << "contents";

  const std::vector<std::wstring> roots{personal.native(), shared.native()};
  const auto documentPath = desktopItem(roots, (personal / L"简报.TXT").native());
  const auto sharedPath = desktopItem(roots, sharedDocument.native());
  check(documentPath == document.native() && sharedPath == sharedDocument.native(), "resolve both desktops");
  check(desktopItem(roots, folder.native()) == folder.native(), "accept desktop folders");
  for (const auto &path : {outside, child, systemFile, personal / L"missing.txt"})
    check(desktopItem(roots, path.native()).empty(), "reject non-desktop or missing items");
  check(desktopItem(roots, (personal / L"简报.txt:stream").native()).empty(), "reject alternate streams");

  check(load(layout).empty() && !fn::fs::exists(layout), "missing settings load as empty");
  std::vector<Group> groups{
      {L"工作 \"😀\"\\", {-450, 10, -110, 320}, {}, true, {{L"常用", {}, {}}, {L"映射", folder.native(), {}}},
       View::List, Sort::Modified, 64, 72, RGB(24, 60, 96), 49, TitleStyle::IconAndText, L"C:\\Box.ico", DisplayMode::Pages},
      {L"共享", {20, 40, 320, 340}, {}, false, {{L"资料", {}, {}}}, View::Icons, Sort::Manual}};
  groups[0].itemSpacing = 14;
  check(addItem(groups, 0, documentPath), "add a document");
  check(!addItem(groups, 0, (personal / L"简报.TXT").native()), "deduplicate case-insensitively");
  check(addItem(groups, 1, sharedPath) && addItem(groups, 1, documentPath), "add and move between groups");
  check(groups[0].paths.empty() && groups[1].paths == std::vector<std::wstring>{sharedPath, documentPath},
        "maintain exclusive membership and order");
  check(addItem(groups, 1, documentPath, 1) && groups[1].paths == std::vector<std::wstring>{sharedPath},
        "move a desktop item into another tab in the same group");
  check(addItem(groups, 0, documentPath, 1) && groups[1].tabs[0].paths.empty(),
        "move a desktop item across groups and tabs");
  check(!addItem(groups, 0, documentPath, 1) && !addItem(groups, 0, documentPath, 2) &&
        !addItem(groups, 0, documentPath, 3), "reject duplicates, mapped tabs and invalid indices");
  save(groups, layout);
  const auto json = readFile(layout);
  check(json.starts_with("{") && json.find("\"version\": 1") != json.npos &&
        json.find("\"iconSize\": 64") != json.npos &&
        json.find("\"sort\": \"modified\"") != json.npos && json.find("工作") != json.npos,
        "write a versioned UTF-8 JSON file");
  constexpr std::string_view displayModeLine = "      \"displayMode\": \"pages\",\n";
  const auto displayModeOffset = json.find(displayModeLine);
  check(displayModeOffset != json.npos && json.find("\"displayMode\"", displayModeOffset + displayModeLine.size()) == json.npos,
        "write display mode only for the paged box");
  constexpr std::string_view spacingLine = "      \"itemSpacing\": 14,\n";
  const auto spacingOffset = json.find(spacingLine);
  check(spacingOffset != json.npos && json.find("\"itemSpacing\"", spacingOffset + spacingLine.size()) == json.npos,
        "write spacing only for the box with extra spacing");
  fn::fs::remove(sharedDocument);
  const auto loaded = load(layout);
  check(loaded.size() == 2 && loaded[0].name == groups[0].name && loaded[1].name == groups[1].name &&
        EqualRect(&loaded[0].bounds, &groups[0].bounds) && EqualRect(&loaded[1].bounds, &groups[1].bounds) &&
        loaded[0].collapsed && !loaded[1].collapsed &&
        loaded[0].view == View::List && loaded[0].sort == Sort::Modified &&
        loaded[1].view == View::Icons && loaded[1].sort == Sort::Manual &&
        loaded[0].displayMode == DisplayMode::Pages && loaded[1].displayMode == DisplayMode::Scroll &&
        loaded[0].itemSpacing == 14 && loaded[1].itemSpacing == 0 &&
        loaded[0].iconSize == 64 && loaded[1].iconSize == defaultIconSize &&
        loaded[0].opacity == 72 && loaded[0].tint == RGB(24, 60, 96) &&
        loaded[0].titleHeight == 49 && loaded[0].titleStyle == TitleStyle::IconAndText &&
        loaded[0].titleIcon == L"C:\\Box.ico" &&
        !loaded[1].opacity && !loaded[1].tint && loaded[1].titleHeight == 36 &&
        loaded[1].titleStyle == TitleStyle::Text && loaded[1].titleIcon.empty() &&
        loaded[0].paths.empty() && loaded[1].paths == groups[1].paths,
        "persist group order, geometry, collapse state, display preferences and missing references");
  check(loaded[0].tabs.size() == 2 && loaded[0].tabs[0].name == L"常用" && loaded[0].tabs[0].folder.empty() &&
        loaded[0].tabs[0].paths == std::vector<std::wstring>{documentPath} &&
        loaded[0].tabs[1].name == L"映射" && loaded[0].tabs[1].folder == folder.native() &&
        loaded[0].tabs[1].paths.empty(), "persist ordered manual and mapped tabs");
  check(fn::fs::exists(document) && fn::fs::exists(folder), "grouping leaves files in place");

  check(titleStyleFor(false, true) == TitleStyle::Text && !showsTitleIcon(TitleStyle::Text) &&
        showsTitleText(TitleStyle::Text), "support title text only");
  check(titleStyleFor(true, false) == TitleStyle::Icon && showsTitleIcon(TitleStyle::Icon) &&
        !showsTitleText(TitleStyle::Icon), "support a title icon only");
  check(titleStyleFor(true, true) == TitleStyle::IconAndText && showsTitleIcon(TitleStyle::IconAndText) &&
        showsTitleText(TitleStyle::IconAndText), "support showing both title elements");
  check(titleStyleFor(false, false) == TitleStyle::Hidden && !showsTitleIcon(TitleStyle::Hidden) &&
        !showsTitleText(TitleStyle::Hidden), "support hiding both title elements");
  auto hiddenTitle = groups[0];
  hiddenTitle.titleStyle = TitleStyle::Hidden;
  const auto hiddenTitleLayout = root / L"hidden-title.json";
  save({hiddenTitle}, hiddenTitleLayout);
  const auto hiddenTitleJson = readFile(hiddenTitleLayout);
  const auto hiddenTitleGroups = load(hiddenTitleLayout);
  check(hiddenTitleJson.find("\"titleStyle\": 3") != hiddenTitleJson.npos && hiddenTitleGroups.size() == 1 &&
        hiddenTitleGroups[0].titleStyle == TitleStyle::Hidden && hiddenTitleGroups[0].name == hiddenTitle.name &&
        hiddenTitleGroups[0].titleIcon == hiddenTitle.titleIcon, "persist hidden titles and their original data");

  const std::string iconSizeLine = "      \"iconSize\": 64,\n";
  const auto iconSizeOffset = json.find(iconSizeLine);
  check(iconSizeOffset != json.npos, "locate icon size in JSON");
  auto oldJson = json;
  oldJson.erase(spacingOffset, spacingLine.size());
  oldJson.erase(iconSizeOffset, iconSizeLine.size());
  oldJson.erase(displayModeOffset, displayModeLine.size());
  constexpr std::string_view looseItemsLine = ",\n  \"looseItems\": []";
  const auto looseItemsOffset = oldJson.find(looseItemsLine);
  check(looseItemsOffset != oldJson.npos, "locate loose items in JSON");
  oldJson.erase(looseItemsOffset, looseItemsLine.size());
  const auto oldLayout = root / L"old.json";
  std::ofstream(oldLayout, std::ios::binary) << oldJson;
  const auto old = loadLayout(oldLayout);
  check(old.groups[0].iconSize == defaultIconSize && old.groups[0].displayMode == DisplayMode::Scroll &&
        old.groups[0].itemSpacing == 0 && old.groups[0].blurStrength == defaultBlurStrength &&
        old.looseItems.empty(), "load older JSON with defaults");

  auto badJson = json;
  badJson.replace(iconSizeOffset, iconSizeLine.size(), "      \"iconSize\": 50,\n");
  const auto badSizeLayout = root / L"bad-icon-size.json";
  std::ofstream(badSizeLayout, std::ios::binary) << badJson;
  bool rejected = false;
  try { load(badSizeLayout); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && readFile(badSizeLayout) == badJson, "reject unsupported icon size without rewriting JSON");

  groups[0].paths = {documentPath};
  rejected = false;
  try { save(groups, layout); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && readFile(layout) == json, "reject duplicate membership across tabs without overwriting layout");

  groups = loaded;
  groups[0].tabs[0].paths.clear();
  groups[0].tabs[1].paths = {documentPath};
  rejected = false;
  try { save(groups, layout); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && load(layout)[0].tabs[1].paths.empty(), "reject manual membership in mapped tab");

  groups = loaded;
  groups[0].bounds = {std::numeric_limits<LONG>::min(), 0, std::numeric_limits<LONG>::max(), 200};
  rejected = false;
  try { save(groups, layout); }
  catch (const fn::Error &) { rejected = true; }
  const auto unchangedBounds = load(layout)[0].bounds;
  check(rejected && EqualRect(&unchangedBounds, &loaded[0].bounds), "reject overflowing geometry");

  // A version 1 group uses the same geometry and strings, with no collapse field.
  const std::uint16_t legacy[] = {1, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 1, 0, L'A', 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, legacy, sizeof(legacy)) == ERROR_SUCCESS,
        "seed a version 1 layout");
  check(load(layout)[0].name == loaded[0].name, "prefer existing JSON over legacy registry");
  const auto legacyFile = root / L"version1.json";
  const auto legacyLayout = load(legacyFile);
  const RECT legacyBounds{5, 7, 405, 307};
  check(legacyLayout.size() == 1 && legacyLayout[0].name == L"A" && !legacyLayout[0].collapsed &&
        legacyLayout[0].tabs.empty() && legacyLayout[0].view == View::Icons && legacyLayout[0].sort == Sort::Manual &&
        EqualRect(&legacyLayout[0].bounds, &legacyBounds) &&
        load(legacyFile)[0].name == L"A", "migrate version 1 groups into JSON with default settings");
  std::uint16_t oldRegistry[ARRAYSIZE(legacy)]{};
  DWORD oldBytes = sizeof(oldRegistry);
  check(RegGetValueW(HKEY_CURRENT_USER, settings, value, RRF_RT_REG_BINARY, nullptr,
                     oldRegistry, &oldBytes) == ERROR_SUCCESS && oldBytes == sizeof(legacy) &&
        std::equal(std::begin(oldRegistry), std::end(oldRegistry), std::begin(legacy)),
        "retain the original legacy registry data after migration");

  const std::uint16_t version2[] = {2, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 1, 0, 1, 0, L'B', 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, version2, sizeof(version2)) == ERROR_SUCCESS,
        "seed a version 2 layout");
  const auto version2Layout = load(root / L"version2.json");
  check(version2Layout.size() == 1 && version2Layout[0].name == L"B" && version2Layout[0].collapsed &&
        version2Layout[0].tabs.empty() && version2Layout[0].view == View::Icons &&
        version2Layout[0].sort == Sort::Manual, "preserve version 2 collapse state with default tabs and display");
  const std::uint16_t version3[] = {
      3, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 1, 0, 1, 0, L'C', 0, 0,
      1, 0, 3, 0, 1, 0, 1, 0, L'D', 0, 0, 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, version3, sizeof(version3)) == ERROR_SUCCESS,
        "seed a version 3 layout");
  const auto version3File = root / L"version3.json";
  const auto version3Layout = load(version3File);
  check(version3Layout.size() == 1 && version3Layout[0].name == L"C" && version3Layout[0].collapsed &&
        version3Layout[0].tabs.size() == 1 && version3Layout[0].tabs[0].name == L"D" &&
        version3Layout[0].view == View::List && version3Layout[0].sort == Sort::Modified,
        "preserve version 3 tabs and display options");
  check(load(legacyFile)[0].name == L"A", "do not overwrite migrated files with later registry data");
  const auto broken = root / L"broken.json";
  std::ofstream(broken, std::ios::binary) << "{\"version\":99,\"groups\":[]}";
  rejected = false;
  try { load(broken); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && readFile(broken) == "{\"version\":99,\"groups\":[]}",
        "leave unsupported JSON intact and do not import legacy over it");

  auto changed = loaded;
  changed[0].name = L"Edited";
  changed[0].opacity = 0;
  save(changed, layout);
  const auto edited = load(layout);
  check(edited[0].name == L"Edited" && readFile(layout) != json,
        "atomically replace the layout file");
  check(edited[0].opacity == 0 && !edited[1].opacity,
        "preserve explicit zero background opacity separately from the default");
  check(!loaded[0].glass && !loaded[1].glass, "layouts without glass overrides inherit the global style");
  check(!loaded[0].autoCollapse && !loaded[1].autoCollapse, "layouts default to manual collapse");
  changed[0].glass = true;
  changed[1].glass = false;
  changed[0].blurStrength = 0;
  changed[1].blurStrength = 100;
  changed[0].autoCollapse = true;
  save(changed, layout);
  const auto glass = load(layout);
  check(glass[0].glass == true && glass[1].glass == false, "persist per-box glass overrides in either direction");
  check(glass[0].blurStrength == 0 && glass[1].blurStrength == 100, "persist zero and maximum blur independently");
  check(glass[0].autoCollapse && !glass[1].autoCollapse, "persist automatic collapse independently for each box");
  const auto validGlassJson = readFile(layout);
  for (const int strength : {-1, 101}) {
    auto invalid = changed;
    invalid[0].blurStrength = strength;
    rejected = false;
    try { save(invalid, layout); }
    catch (const fn::Error &) { rejected = true; }
    check(rejected && readFile(layout) == validGlassJson, "reject invalid blur without overwriting saved settings");
    auto invalidJson = validGlassJson;
    const auto field = invalidJson.find("\"blurStrength\": 0");
    check(field != invalidJson.npos, "explicit zero blur is serialized");
    invalidJson.replace(field, std::string_view("\"blurStrength\": 0").size(), "\"blurStrength\": " + std::to_string(strength));
    const auto invalidFile = root / L"invalid-blur.json";
    std::ofstream(invalidFile, std::ios::binary) << invalidJson;
    rejected = false;
    try { load(invalidFile); }
    catch (const fn::Error &) { rejected = true; }
    check(rejected && readFile(invalidFile) == invalidJson, "reject out-of-range blur on load without rewriting it");
  }
  changed[1].glass.reset();
  save(changed, layout);
  check(!load(layout)[1].glass, "clearing a glass override restores inheritance");

  const auto looseFile = root / L"loose.json";
  Layout withLoose{loaded, {{folder.native(), {-820, 320}}, {(personal / L"其他.txt").native(), {800, -100}, {98, 137}}}};
  saveLayout(withLoose, looseFile);
  const auto looseLayout = loadLayout(looseFile);
  check(looseLayout.groups.size() == loaded.size() && looseLayout.looseItems.size() == 2 &&
        looseLayout.looseItems[0].path == folder.native() && looseLayout.looseItems[0].position.x == -820 &&
        looseLayout.looseItems[0].position.y == 320 && looseLayout.looseItems[1].position.y == -100 &&
        looseLayout.looseItems[0].spacing.cx == 0 && looseLayout.looseItems[1].spacing.cx == 98 &&
        looseLayout.looseItems[1].spacing.cy == 137,
        "preserve loose desktop paths and virtual screen coordinates");
  withLoose.looseItems.push_back({documentPath, {}});
  rejected = false;
  try { saveLayout(withLoose, looseFile); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && loadLayout(looseFile).looseItems.size() == 2, "reject items both loose and grouped");
  withLoose.looseItems.back().path = (personal / L"其他.TXT").native();
  rejected = false;
  try { saveLayout(withLoose, looseFile); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected && loadLayout(looseFile).looseItems.size() == 2, "reject duplicate loose paths");
  auto oldApiGroups = looseLayout.groups;
  oldApiGroups[0].paths.push_back(folder.native());
  save(oldApiGroups, looseFile);
  const auto updatedLoose = loadLayout(looseFile);
  check(updatedLoose.groups[0].paths == std::vector<std::wstring>{folder.native()} &&
        updatedLoose.looseItems.size() == 1 && updatedLoose.looseItems[0].path == looseLayout.looseItems[1].path,
        "preserve unaffected loose placements when saving through the group-only API");

  std::vector<Group> stack = loaded;
  stack[0].paths.push_back(folder.native());
  std::vector<Group> empty;
  check(!mergeGroups(empty, 0, 1) && !mergeGroups(stack, 0, 2) && !mergeGroups(stack, 2, 0) &&
        !mergeGroups(stack, 0, 0) && stack.size() == 2, "reject invalid merges");
  auto intoFirst = loaded;
  check(mergeGroups(intoFirst, 1, 0) && intoFirst.size() == 1 && intoFirst.front().name == loaded[0].name &&
        intoFirst.front().tabs.back().name == L"资料", "merge into the first box");
  check(mergeGroups(stack, 0, 1) && stack.size() == 1 &&
        stack[0].name == loaded[1].name && stack[0].paths == loaded[1].paths && stack[0].tabs.size() == 4 &&
        stack[0].tabs[1].name == loaded[0].name && stack[0].tabs[1].paths == std::vector<std::wstring>{folder.native()} &&
        stack[0].tabs[2].paths == std::vector<std::wstring>{documentPath} &&
        stack[0].tabs[3].folder == folder.native(), "merge the first box's default, manual and mapped tabs in order");
  stack[0].view = View::List;
  stack[0].sort = Sort::Modified;
  stack[0].iconSize = 64;
  stack[0].displayMode = DisplayMode::Pages;
  stack[0].itemSpacing = maxItemSpacing;
  stack[0].glass = false;
  stack[0].blurStrength = 73;
  stack[0].autoCollapse = true;
  const RECT detachedBounds{-500, 40, -110, 370};
  check(!detachTab(empty, 0, 1, detachedBounds) && !detachTab(stack, 1, 1, detachedBounds) &&
        !detachTab(stack, 0, 5, detachedBounds) &&
        !detachTab(stack, 0, 1, {}), "reject invalid detaches");
  check(detachTab(stack, 0, 4, detachedBounds) && stack.size() == 2 && stack[0].tabs.size() == 3 &&
        stack.back().name == L"映射" && stack.back().paths.empty() && stack.back().tabs.empty() &&
        stack.back().folder == folder.native() && EqualRect(&stack.back().bounds, &detachedBounds),
        "detach a mapped tab into the only tab of a new box");
  check(!addItem(stack, 1, documentPath) && stack[0].tabs[2].paths == std::vector<std::wstring>{documentPath},
        "reject a manual assignment to a mapped main tab without disturbing the existing assignment");
  check(detachTab(stack, 0, 3, detachedBounds) && stack.size() == 3 && stack[0].tabs.size() == 2 &&
        stack.back().name == L"常用" && stack.back().paths == std::vector<std::wstring>{documentPath} &&
        stack.back().tabs.empty() && stack.back().view == View::List && stack.back().sort == Sort::Modified &&
        stack.back().iconSize == 64 && stack.back().displayMode == DisplayMode::Pages &&
        stack.back().itemSpacing == maxItemSpacing && stack.back().glass == false && stack.back().blurStrength == 73 && stack.back().autoCollapse &&
        !stack.back().collapsed,
        "detach manual tab and retain display preferences");
  const auto stackFile = root / L"stack.json";
  save(stack, stackFile);
  auto savedStack = load(stackFile);
  check(savedStack.size() == 3 && savedStack[1].folder == folder.native() && savedStack[1].tabs.empty() &&
        savedStack[0].folder.empty() && savedStack[2].folder.empty(),
        "roundtrip the mapped primary tab and ordinary manual boxes");
  check(mergeGroups(savedStack, 1, 0) && savedStack.front().tabs.back().name == L"映射" &&
        savedStack.front().tabs.back().folder == folder.native() && savedStack.front().tabs.back().paths.empty(),
        "merge a standalone mapped box back as a mapped tab");

  std::vector<Group> legacyMapped{
      {L"Mapped", detachedBounds, {}, false, {{L"Mapped", folder.native(), {}}}},
      {L"Manual", detachedBounds, {}, false, {{L"Manual", {}, {}}}},
      {L"Different", detachedBounds, {}, false, {{L"Mapped", folder.native(), {}}}},
      {L"Retain", detachedBounds, {}, false, {{L"Retain", folder.native(), {}}, {L"Extra", {}, {}}}},
      {L"Retain tint", detachedBounds, {}, false, {{L"Retain tint", folder.native(), {}, RGB(42, 63, 84)}}},
      {L"With items", detachedBounds, {documentPath}, false, {{L"With items", folder.native(), {}}}}};
  const auto legacyMappedFile = root / L"legacy-mapped.json";
  save(legacyMapped, legacyMappedFile);
  auto promoted = load(legacyMappedFile);
  check(promoted.size() == legacyMapped.size() && promoted[0].folder == folder.native() && promoted[0].tabs.empty(),
        "promote the single duplicate legacy mapped tab on load");
  for (size_t i = 1; i < legacyMapped.size(); ++i)
    check(promoted[i].folder.empty() && promoted[i].tabs.size() == legacyMapped[i].tabs.size() &&
          promoted[i].tabs.front().name == legacyMapped[i].tabs.front().name &&
          promoted[i].tabs.front().folder == legacyMapped[i].tabs.front().folder,
          "preserve non-duplicate legacy tabs");

  const auto sourceTint = RGB(0, 0, 0), hostTint = RGB(70, 92, 114);
  std::vector<Group> nested{
      {},
      {L"Original", detachedBounds, {}, false, {{L"Original extra", {}, {}}, {L"Mapped", folder.native(), {}}}},
      {L"Default", detachedBounds, {}, false, {{L"Default extra", {}, {}}}},
      {L"Host", detachedBounds, {}, false, {{L"Host extra", {}, {}}}}};
  nested[1].tint = sourceTint;
  nested[3].tint = hostTint;
  check(mergeGroups(nested, 1, 2) && mergeGroups(nested, 1, 2) && nested[1].tint == hostTint,
        "merging keeps the host color with nested source tabs attached");
  const auto nestedFile = root / L"nested-tints.json";
  save({nested[1]}, nestedFile);
  const auto nestedJson = readFile(nestedFile);
  check(nestedJson.find("\"detachedTint\": null") != nestedJson.npos &&
        nestedJson.find("\"detachedTint\": 0") != nestedJson.npos,
        "persist theme default and custom black distinctly");
  nested[1] = load(nestedFile).front();
  nested[1].tint = RGB(105, 85, 64);
  const auto checkDetachedTint = [&](size_t tab, const wchar_t *name, std::optional<COLORREF> tint) {
    check(detachTab(nested, 1, tab, detachedBounds) && nested.back().name == name && nested.back().tint == tint,
          "detaching restores original tint or inherits the host color for a regular tab");
  };
  checkDetachedTint(6, L"Mapped", sourceTint);
  check(nested.back().folder == folder.native() && nested.back().tabs.empty(),
        "detaching a mapped tab retains its folder and consumes its original color");
  checkDetachedTint(5, L"Original extra", sourceTint);
  checkDetachedTint(4, L"Original", sourceTint);
  checkDetachedTint(3, L"Default extra", std::nullopt);
  checkDetachedTint(2, L"Default", std::nullopt);
  checkDetachedTint(1, L"Host extra", nested[1].tint);

  std::vector<Group> movingTabs{
      {L"Source", detachedBounds, {sharedPath}, false,
       {{L"First", {}, {documentPath}}, {L"Mapped", folder.native(), {}}, {L"Last", {}, {}}}},
      {L"Standalone", detachedBounds, {}, false},
      {L"Destination", detachedBounds, {}, false, {{L"Existing", {}, {}}}}};
  movingTabs[0].tint = sourceTint;
  movingTabs[1].tint = hostTint;
  movingTabs[2].tint = RGB(105, 85, 64);
  const auto sourceOrder = [&] {
    std::vector<std::wstring> names{movingTabs[0].name};
    for (const auto &tab : movingTabs[0].tabs) names.push_back(tab.name);
    return names;
  };
  const std::vector<std::wstring> originalOrder{L"Source", L"First", L"Mapped", L"Last"};
  check(!moveTab(movingTabs, 1, 0, 0, 0) && !detachTab(movingTabs, 1, 0, detachedBounds) &&
        !moveTab(movingTabs, 0, 4, 1, 0) && !moveTab(movingTabs, 0, 1, 1, 2) &&
        movingTabs[0].paths == std::vector<std::wstring>{sharedPath} &&
        sourceOrder() == originalOrder && movingTabs[1].tabs.empty(),
        "reject moving or detaching a box's only tab and invalid header positions");
  check(!moveTab(movingTabs, 0, 0, 0, 0) && !moveTab(movingTabs, 0, 0, 0, 1) &&
        !moveTab(movingTabs, 0, 2, 0, 2) && !moveTab(movingTabs, 0, 2, 0, 3) &&
        sourceOrder() == originalOrder, "dropping on either edge of the same tab is a no-op");
  check(moveTab(movingTabs, 0, 1, 0, 4) &&
        sourceOrder() == std::vector<std::wstring>{L"Source", L"Mapped", L"Last", L"First"} &&
        movingTabs[0].tabs.back().paths == std::vector<std::wstring>{documentPath} &&
        !movingTabs[0].tabs.back().detachedTint, "reorder rightward using the trailing original slot");
  check(moveTab(movingTabs, 0, 3, 0, 1) && sourceOrder() == originalOrder,
        "reorder an additional tab leftward using the original slot");
  check(moveTab(movingTabs, 0, 0, 0, 2) &&
        sourceOrder() == std::vector<std::wstring>{L"First", L"Source", L"Mapped", L"Last"} &&
        movingTabs[0].paths == std::vector<std::wstring>{documentPath} &&
        movingTabs[0].tabs.front().paths == std::vector<std::wstring>{sharedPath},
        "move the primary tab right and promote its neighbor with the correct contents");
  check(moveTab(movingTabs, 0, 1, 0, 0) && sourceOrder() == originalOrder &&
        movingTabs[0].paths == std::vector<std::wstring>{sharedPath},
        "move the former primary left to its original slot");
  check(moveTab(movingTabs, 0, 2, 0, 0) && moveTab(movingTabs, 0, 0, 1, 0) &&
        sourceOrder() == std::vector<std::wstring>{L"Source", L"First", L"Last"} &&
        movingTabs[0].paths == std::vector<std::wstring>{sharedPath} && movingTabs[0].folder.empty() &&
        movingTabs[1].name == L"Mapped" && movingTabs[1].folder == folder.native() &&
        movingTabs[1].tabs.size() == 1 && movingTabs[1].tabs.front().name == L"Standalone" &&
        movingTabs[1].tint == hostTint,
        "move a mapped primary to the first slot of a standalone box and promote the source's next tab");
  const auto movedTabsFile = root / L"moved-tabs.json";
  save(movingTabs, movedTabsFile);
  movingTabs = load(movedTabsFile);
  check(moveTab(movingTabs, 1, 0, 2, 0) && movingTabs[1].name == L"Standalone" && movingTabs[1].tabs.empty() &&
        movingTabs[2].name == L"Mapped" && movingTabs[2].folder == folder.native() &&
        movingTabs[2].tabs.size() == 2 && movingTabs[2].tabs[0].name == L"Destination" &&
        movingTabs[2].tabs[1].name == L"Existing",
        "reload the mapped primary and move it in front of another box's original primary");
  check(detachTab(movingTabs, 2, 0, detachedBounds) && movingTabs[2].name == L"Destination" &&
        movingTabs[2].folder.empty() && movingTabs[2].tabs.size() == 1 &&
        movingTabs.back().name == L"Mapped" &&
        movingTabs.back().folder == folder.native() && movingTabs.back().tabs.empty() &&
        movingTabs.back().tint == sourceTint,
        "detaching a transferred primary keeps its mapped folder and original source tint");
  check(detachTab(movingTabs, 0, 0, detachedBounds) && movingTabs[0].name == L"First" &&
        movingTabs[0].paths == std::vector<std::wstring>{documentPath} &&
        movingTabs.back().name == L"Source" && movingTabs.back().paths == std::vector<std::wstring>{sharedPath} &&
        movingTabs.back().tint == sourceTint,
        "detaching the manual primary preserves both tabs' distinct file assignments");

  {
    std::vector<Group> deletingTabs{
        {L"BoxA", detachedBounds, {sharedPath}, false,
         {{L"BoxB", {}, {documentPath}, std::optional<COLORREF>{hostTint}},
          {L"MappedC", folder.native(), {}, std::nullopt}}},
        {L"Single", detachedBounds, {}, false}};
    deletingTabs[0].tint = sourceTint;
    deletingTabs[0].autoRule = L"*.png";
    check(!deleteTab(deletingTabs, 1, 0) && !deleteTab(deletingTabs, 0, 3) && !deleteTab(deletingTabs, 9, 0),
          "reject deleting a single-tab box or out-of-bounds tab");
    const auto removedMapped = deleteTab(deletingTabs, 0, 2);
    check(removedMapped && removedMapped->name == L"MappedC" && removedMapped->folder == folder.native() &&
          deletingTabs[0].name == L"BoxA" && deletingTabs[0].tabs.size() == 1 &&
          deletingTabs[0].tint == sourceTint && deletingTabs[0].autoRule == L"*.png",
          "deleting a secondary tab keeps the remaining tabs and primary box intact");
    const auto removedPrimary = deleteTab(deletingTabs, 0, 0);
    check(removedPrimary && removedPrimary->name == L"BoxA" &&
          removedPrimary->paths == std::vector<std::wstring>{sharedPath} &&
          deletingTabs[0].name == L"BoxB" &&
          deletingTabs[0].paths == std::vector<std::wstring>{documentPath} &&
          deletingTabs[0].tabs.empty() && deletingTabs[0].autoRule.empty() &&
          deletingTabs[0].tint == hostTint && !deletingTabs[0].primaryDetachedTint,
          "deleting the primary tab promotes the survivor and restores its standalone tint");
  }

  const std::uint16_t invalidState[] = {2, 0, 1, 0, 5, 0, 7, 0, 405, 0, 307, 0, 2, 0, 1, 0, L'A', 0, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, invalidState,
                        sizeof(invalidState)) == ERROR_SUCCESS, "seed an invalid collapse state");
  rejected = false;
  try { load(root / L"invalid-state.json"); }
  catch (const fn::Error &) { rejected = true; }
  check(rejected, "reject invalid collapse state");

  const DWORD unsupported[] = {0xFFFFFFFF, 0};
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, value, REG_BINARY, unsupported,
                        sizeof(unsupported)) == ERROR_SUCCESS, "seed an unsupported layout version");
  rejected = false;
  try { load(root / L"unsupported-legacy.json"); }
  catch (const fn::Error &) { rejected = true; }
  DWORD unchanged[2]{}, bytes = sizeof(unchanged);
  check(rejected && RegGetValueW(HKEY_CURRENT_USER, settings, value, RRF_RT_REG_BINARY, nullptr,
                                unchanged, &bytes) == ERROR_SUCCESS &&
        unchanged[0] == unsupported[0], "reject unsupported layout without clearing it");
}

void exerciseNewBoxEnhancements(const fn::fs::path &root) {
  using namespace desktop_groups;
  fn::fs::create_directories(root);

  // 1. Auto-grouping rule matching
  check(matchesAutoRule(L"Photo.PNG", L".png, .jpg"), "match leading-dot extension case-insensitively");
  check(matchesAutoRule(L"截图_20261123.jpg", L"截图*；*.pdf"), "match wildcard prefix and Chinese semicolon");
  check(matchesAutoRule(L"2026电子发票.pdf", L"发票"), "match plain keyword substring");
  check(!matchesAutoRule(L"archive.zip", L".png, .jpg, 截图*, 发票"), "reject non-matching filenames");

  // 2. Proportional multi-monitor workArea scaling
  const RECT work2k{0, 0, 2560, 1400};
  const RECT work1080{0, 0, 1920, 1040};
  const RECT rightAligned{2200, 20, 2560, 320};
  const auto scaledRight = scaleBoundsToWorkArea(rightAligned, work2k, work1080);
  check(scaledRight.right == 1920 && scaledRight.right - scaledRight.left == 360,
        "right-aligned box stays flush with the right edge on smaller monitor");
  const auto restoredRight = scaleBoundsToWorkArea(rightAligned, work2k, work2k);
  check(EqualRect(&restoredRight, &rightAligned), "reconnecting original monitor preserves exact pixel bounds");
  const RECT centered{1100, 550, 1460, 850};
  const auto scaledCenter = scaleBoundsToWorkArea(centered, work2k, work1080);
  check(std::abs((scaledCenter.left + scaledCenter.right) / 2 - 960) <= 1 &&
        std::abs((scaledCenter.top + scaledCenter.bottom) / 2 - 520) <= 1,
        "centered box stays centered across resolution changes");

  // 3. Model persistence for hoverMenu, autoRule, workArea + Backup export/import
  Group custom{L"自动收纳", {100, 120, 460, 400}, {}, false, {{L"子标签", {}, {}}}};
  custom.hoverMenu = false;
  custom.autoRule = L".png, 截图*";
  custom.referenceWorkArea = work2k;
  const auto layoutFile = root / L"enhanced.json";
  save({custom}, layoutFile);
  auto reloaded = load(layoutFile);
  check(reloaded.size() == 1 && !reloaded[0].hoverMenu && reloaded[0].autoRule == L".png, 截图*" &&
        EqualRect(&reloaded[0].referenceWorkArea, &work2k),
        "persist hoverMenu, autoRule, and referenceWorkArea");
  check(detachTab(reloaded, 0, 1, {200, 200, 520, 460}) && !reloaded.back().hoverMenu &&
        EqualRect(&reloaded.back().referenceWorkArea, &work2k),
        "detached tab inherits hoverMenu and referenceWorkArea");

  const DWORD customRadius = 17;
  check(RegSetKeyValueW(HKEY_CURRENT_USER, settings, L"DesktopGroupsRadius", REG_DWORD,
                        &customRadius, sizeof(customRadius)) == ERROR_SUCCESS, "seed radius setting");
  const wchar_t pinsMulti[] = L"C:\\Work\0D:\\Docs\0";
  check(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit\\QuickFolders", L"Pins", REG_MULTI_SZ,
                        pinsMulti, sizeof(pinsMulti)) == ERROR_SUCCESS, "seed QuickFolders Pins");
  const auto backupFile = root / L"ShibaKit-Backup.json";
  exportDesktopBackup(backupFile, layoutFile);
  check(fn::fs::exists(backupFile), "write backup JSON file");

  const DWORD zeroRadius = 0;
  RegSetKeyValueW(HKEY_CURRENT_USER, settings, L"DesktopGroupsRadius", REG_DWORD, &zeroRadius, sizeof(zeroRadius));
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\ShibaKit\\QuickFolders");
  const auto restoredLayoutFile = root / L"restored.json";
  const auto imported = importDesktopBackup(backupFile, restoredLayoutFile);
  DWORD restoredRadius = 0, radiusBytes = sizeof(restoredRadius);
  check(imported.groups.size() == 1 && imported.groups[0].autoRule == L".png, 截图*" &&
        RegGetValueW(HKEY_CURRENT_USER, settings, L"DesktopGroupsRadius", RRF_RT_REG_DWORD,
                     nullptr, &restoredRadius, &radiusBytes) == ERROR_SUCCESS && restoredRadius == 17,
        "import backup restores layout and DWORD settings");
  wchar_t restoredPins[64]{};
  DWORD pinsBytes = sizeof(restoredPins);
  check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit\\QuickFolders", L"Pins", RRF_RT_REG_MULTI_SZ,
                     nullptr, restoredPins, &pinsBytes) == ERROR_SUCCESS &&
        std::memcmp(restoredPins, pinsMulti, sizeof(pinsMulti)) == 0,
        "import backup restores QuickFolders multi-string pins");

  // 4. Box item creation & WIC HBITMAP-to-PNG encoding
  struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
  } com;
  const auto personal = root / L"PersonalDesktop";
  fn::fs::create_directories(personal);
  const std::vector<std::wstring> roots{personal.native()};
  const auto folder1 = createNewFolderInBox(roots, L"新建文件夹");
  const auto folder2 = createNewFolderInBox(roots, L"新建文件夹");
  check(!folder1.empty() && !folder2.empty() && folder1 != folder2 &&
        fn::fs::is_directory(folder1) && fn::fs::is_directory(folder2),
        "create collision-safe new folders inside .desktop");
  const auto txt1 = createNewTextFileInBox(roots, L"新建文本文档.txt");
  const auto txt2 = createNewTextFileInBox(roots, L"新建文本文档.txt");
  check(!txt1.empty() && !txt2.empty() && txt1 != txt2 &&
        fn::fs::is_regular_file(txt1) && fn::fs::is_regular_file(txt2),
        "create collision-safe new text files inside .desktop");

  BITMAPINFO bmi{};
  bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), 16, -16, 1, 32, BI_RGB};
  void *bits = nullptr;
  const std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> testBmp(
      CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0), DeleteObject);
  check(testBmp && bits, "allocate test screenshot bitmap");
  std::fill_n(static_cast<DWORD *>(bits), 16 * 16, 0x002060c0u);
  const auto pngPath = saveHBitmapToPngInFolder(testBmp.get(), folder1, L"截图");
  check(!pngPath.empty() && fn::fs::is_regular_file(pngPath), "encode screenshot bitmap to PNG file");
  std::ifstream pngIn(pngPath, std::ios::binary);
  unsigned char header[8]{};
  pngIn.read(reinterpret_cast<char *>(header), sizeof(header));
  const unsigned char expectedPng[8]{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  check(pngIn.gcount() == 8 && std::memcmp(header, expectedPng, 8) == 0, "written screenshot has valid PNG header");
}

void exerciseTransferProgress(const fn::fs::path &root) {
  using namespace desktop_groups;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const auto source = root / L"source";
  const auto target = root / L"target";
  fn::fs::create_directories(source);
  fn::fs::create_directories(target);
  const auto file = source / L"进度.txt";
  {
    std::ofstream out(file, std::ios::binary);
    const std::string block(4096, 'x');
    for (int i = 0; i < 64; ++i) out << block;
  }
  std::vector<double> reports;
  const auto copied = transferItemToFolder(file.native(), target.native(), false, nullptr, true,
                                           [&](double ratio) { reports.push_back(ratio); });
  check(!copied.empty() && fn::fs::equivalent(copied, target / file.filename()) && fn::fs::exists(file),
        "copy keeps the source and reports the destination path");
  check(std::all_of(reports.begin(), reports.end(), [](double ratio) { return ratio >= 0.0 && ratio <= 1.0; }),
        "transfer progress ratios stay within 0..1");
  const auto renamed = transferItemToFolder(file.native(), target.native(), false, nullptr, true);
  check(!renamed.empty() && !fn::fs::equivalent(renamed, copied) && fn::fs::exists(renamed),
        "a copy into an occupied name is renamed instead of overwriting");
  const auto moved = transferItemToFolder(file.native(), target.native(), true, nullptr, true);
  check(!moved.empty() && fn::fs::exists(moved) && !fn::fs::exists(file),
        "a move removes the source and lands in the destination");
  check(transferItemToFolder((source / L"missing.txt").native(), target.native(), false, nullptr, true).empty(),
        "a missing source reports no destination");
  if (SUCCEEDED(com)) CoUninitialize();
}

void exercisePanelAndDesktopFlows(const fn::fs::path &root) {
  using namespace desktop_groups;
  using namespace desktop_groups::detail;
  const HRESULT ole = OleInitialize(nullptr);
  struct OleScope {
    HRESULT hr;
    ~OleScope() { if (SUCCEEDED(hr)) OleUninitialize(); }
  } oleScope{ole};

  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW panelCls{};
  if (!GetClassInfoW(instance, panelClass, &panelCls)) {
    panelCls.lpfnWndProc = Panel::proc;
    panelCls.hInstance = instance;
    panelCls.lpszClassName = panelClass;
    panelCls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    panelCls.style = CS_DBLCLKS;
    check(RegisterClassW(&panelCls) != 0, "register Panel window class for flow tests");
  }
  WNDCLASSW receiverCls{};
  if (!GetClassInfoW(instance, receiverClass, &receiverCls)) {
    receiverCls.lpfnWndProc = Desktop::proc;
    receiverCls.hInstance = instance;
    receiverCls.lpszClassName = receiverClass;
    receiverCls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    check(RegisterClassW(&receiverCls) != 0, "register Desktop receiver window class for flow tests");
  }

  const auto personal = root / L"PersonalDesktop";
  const auto externalDir = root / L"ExternalSource";
  const auto layoutFile = root / L"flow-layout.json";
  fn::fs::create_directories(personal);
  fn::fs::create_directories(externalDir);

  Group group;
  group.name = L"主标签";
  group.bounds = {-31800, -31800, -31440, -31540};
  group.tabs.push_back(Tab{L"待删标签", {}, {}});
  group.tabs.push_back(Tab{L"保留标签", {}, {}});
  save({group}, layoutFile);

  HWND host = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
                              -32000, -32000, 1200, 800, nullptr, nullptr, instance, nullptr);
  check(host != nullptr, "create hidden desktop host window for Panel flow tests");
  struct HostScope {
    HWND handle;
    ~HostScope() { if (handle && IsWindow(handle)) DestroyWindow(handle); }
  } hostScope{host};

  Desktop desktop(instance, layoutFile, std::vector<std::wstring>{personal.native()});
#ifdef SHIBAKIT_WINUI
  desktop.useWinui = false;
#endif
  desktop.host = host;

  check(CreateWindowExW(WS_EX_TOOLWINDOW, receiverClass, L"", WS_POPUP, 0, 0, 0, 0,
                        nullptr, nullptr, instance, &desktop) != nullptr && desktop.window != nullptr,
        "create Desktop receiver window");
  struct DesktopScope {
    Desktop &d;
    ~DesktopScope() {
      d.quitting = true;
      d.panels.clear();
      if (d.window && IsWindow(d.window)) DestroyWindow(d.window);
    }
  } desktopScope{desktop};

  check(desktop.openPanel(0) && desktop.panels.size() == 1, "open hidden Panel attached to test host");
  auto &panel = *desktop.panels[0];

  // 1. Power-saving headerTimer auto-stop when idle and immediate wake on mouse activity
  desktop.ensureHeaderTimer();
  check(desktop.headerTimerRunning, "ensureHeaderTimer starts the 70ms header polling timer");
  desktop.tickHeader();
  check(!desktop.headerTimerRunning, "tickHeader stops headerTimer automatically when cursor is outside all boxes and idle");
  SendMessageW(panel.window, WM_SETCURSOR, reinterpret_cast<WPARAM>(panel.window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
  check(desktop.headerTimerRunning, "mouse activity over a box immediately wakes headerTimer");
  SendMessageW(desktop.window, WM_TIMER, headerTimer, 0);
  check(!desktop.headerTimerRunning, "subsequent headerTimer tick stops timer again once cursor is outside and idle");

  // 2. Optimistic transfer progress placeholder & completion into Panel
  const auto externalFile = externalDir / L"异步传入.txt";
  {
    std::ofstream out(externalFile, std::ios::binary);
    out << "transfer-test-payload";
  }
  check(desktop.enqueueAsyncTransferIntoPanel(panel, externalFile.native(), 0, false),
        "enqueueAsyncTransferIntoPanel accepts external file");
  check(panel.activeTransfers.size() == 1 && panel.activeTransfers[0].placeholder &&
        !panel.orderedItems.empty() && isTransferToken(panel.orderedItems[0]) &&
        desktop.transferTimerRunning,
        "optimistic transfer inserts placeholder token into Panel and starts transferTimer");

  const auto deadline = GetTickCount64() + 4000;
  while (GetTickCount64() < deadline) {
    desktop.handleCompletedTransfers();
    if (!panel.activeTransfers.empty() && panel.activeTransfers[0].finished) break;
    Sleep(15);
  }
  check(!panel.activeTransfers.empty() && panel.activeTransfers[0].finished,
        "background worker completes file transfer into .desktop");
  panel.activeTransfers[0].visualProgress = 1.0;
  panel.activeTransfers[0].finishedAt = GetTickCount64() - 100;
  desktop.tickTransfers();
  check(panel.activeTransfers.empty() && !desktop.transferTimerRunning &&
        !panel.orderedItems.empty() && !isTransferToken(panel.orderedItems[0]) &&
        fn::fs::exists(panel.orderedItems[0]),
        "completed transfer replaces placeholder with stored .desktop path and stops transferTimer");

  // 3. Multi-tab targeted tab removal updates tabs, activeTab, and activeTransfers tab indices
  panel.showTab(2);
  check(panel.activeTab == 2, "switch Panel to third tab before removing middle tab");
  Panel::TransferEntry dummyOnRemovedTab;
  dummyOnRemovedTab.id = 9001;
  dummyOnRemovedTab.tab = 1;
  dummyOnRemovedTab.placeholder = false;
  dummyOnRemovedTab.finished = true;
  dummyOnRemovedTab.finishedAt = GetTickCount64();
  Panel::TransferEntry dummyOnSurvivingTab = dummyOnRemovedTab;
  dummyOnSurvivingTab.id = 9002;
  dummyOnSurvivingTab.tab = 2;
  panel.activeTransfers.push_back(dummyOnRemovedTab);
  panel.activeTransfers.push_back(dummyOnSurvivingTab);

  SendMessageW(panel.window, WM_COMMAND, RemoveTab, 1);
  check(desktop.groups[0].tabs.size() == 1 && desktop.groups[0].tabs[0].name == L"保留标签" &&
        panel.activeTab == 1 &&
        panel.activeTransfers.size() == 1 && panel.activeTransfers[0].id == 9002 &&
        panel.activeTransfers[0].tab == 1,
        "removing non-active middle tab via Panel WM_COMMAND preserves other tabs and shifts activeTab/transfers");
  panel.activeTransfers.clear();

  // 4. In-box middle-click fallback gesture tracking on Panel and ListView child
  SendMessageW(panel.window, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(12, 12));
  check(panel.middleDown, "Panel WM_MBUTTONDOWN records middle-click press");
  panel.middleDownPoint = {-30000, -30000};
  SendMessageW(panel.window, WM_MBUTTONUP, 0, MAKELPARAM(12, 12));
  check(!panel.middleDown, "Panel WM_MBUTTONUP clears middle-click state");

  SendMessageW(panel.list, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(12, 12));
  check(panel.middleDown, "ListView child WM_MBUTTONDOWN records middle-click press via iconInput");
  panel.middleDownPoint = {-30000, -30000};
  SendMessageW(panel.list, WM_MBUTTONUP, 0, MAKELPARAM(12, 12));
  check(!panel.middleDown, "ListView child WM_MBUTTONUP clears middle-click state");

  // 5. Drag-out moved file no longer on disk does not post a false rejectedMessage
  const std::wstring movedOutPath = (personal / L".desktop" / L"已拖出到桌面.pdf").native();
  desktop.assign(panel, std::span<const std::wstring>(&movedOutPath, 1), {-1, -1}, false);
  MSG rejectedMsg{};
  check(!PeekMessageW(&rejectedMsg, panel.window, rejectedMessage, rejectedMessage, PM_REMOVE),
        "assigning a path already moved out of .desktop does not post a false error dialog");
}

void exerciseBoxSurfacePalette() {
  using namespace desktop_groups;
  const ui::Palette lightBase{RGB(240, 240, 240), RGB(255, 255, 255), RGB(255, 255, 255), RGB(229, 229, 229),
                              RGB(214, 214, 214), RGB(214, 214, 214), RGB(12, 12, 12), RGB(110, 110, 110),
                              RGB(0, 120, 215), RGB(255, 255, 255), false, false};
  const auto fromLight = boxSurfacePalette(lightBase);
  check(fromLight.dark && fromLight.text == RGB(255, 255, 255) && fromLight.muted == RGB(204, 204, 204),
        "light system theme forces pure white text and light muted text on desktop boxes");

  const ui::Palette darkBase{RGB(25, 25, 25), RGB(32, 32, 32), RGB(32, 32, 32), RGB(53, 53, 53),
                             RGB(64, 64, 64), RGB(64, 64, 64), RGB(240, 240, 240), RGB(166, 166, 166),
                             RGB(0, 120, 215), RGB(255, 255, 255), true, false};
  const auto fromDark = boxSurfacePalette(darkBase);
  check(fromDark.dark && fromDark.text == RGB(255, 255, 255) && fromDark.muted == RGB(204, 204, 204),
        "dark system theme uses pure white text and light muted text on desktop boxes");

  const ui::Palette hcBase{RGB(0, 0, 0), RGB(0, 0, 0), RGB(0, 0, 0), RGB(30, 144, 255), RGB(30, 144, 255),
                           RGB(255, 255, 255), RGB(255, 255, 0), RGB(192, 192, 192), RGB(30, 144, 255),
                           RGB(0, 0, 0), false, true};
  const auto keptHc = boxSurfacePalette(hcBase);
  check(keptHc.highContrast && keptHc.text == hcBase.text, "high contrast mode preserves explicit system colors");
}
}

int main() {
  try {
    const TemporaryFiles files;
    exerciseShellIdListDrop(files.root / L"shell-drop-test");
    TemporaryRegistry registry;
    registry.redirect(files.root.filename().native());
    exercise(files.root);
    exerciseBoxStorage(files.root / L"storage-test");
    exerciseNewBoxEnhancements(files.root / L"enhancements-test");
    exerciseTransferProgress(files.root / L"transfer-test");
    exercisePanelAndDesktopFlows(files.root / L"panel-flow-test");
    exerciseBoxSurfacePalette();
    exerciseShadow();
    exerciseThumbnailMask();
    exerciseThumbnailFit(files.root);
  } catch (const fn::Error &error) {
    std::wcerr << error.message << L'\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
