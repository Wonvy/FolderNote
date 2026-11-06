#include "../src/quick_icons.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>
namespace {
int checks = 0;
void check(bool ok, const char *why) { ++checks; if (!ok) throw std::runtime_error(why); }
std::vector<BYTE> pixels(HICON icon) {
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = -16;
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr; HDC dc = CreateCompatibleDC(nullptr);
  HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  auto old = SelectObject(dc, bitmap); memset(bits, 0, 16 * 16 * 4);
  DrawIconEx(dc, 0, 0, icon, 16, 16, 0, nullptr, DI_NORMAL); GdiFlush();
  std::vector<BYTE> result(static_cast<BYTE *>(bits), static_cast<BYTE *>(bits) + 16 * 16 * 4);
  // GDI does not promise meaningful destination alpha; compare visible RGB.
  for (size_t i = 3; i < result.size(); i += 4) result[i] = 255;
  SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); return result;
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  int started = 0, finished = 0;
  bool release = false;
};
}
int main() {
  using namespace quick::icons;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    GUID guid{}; CoCreateGuid(&guid); wchar_t id[40]{}; StringFromGUID2(guid, id, ARRAYSIZE(id));
    auto root = std::filesystem::temp_directory_path() / (std::wstring(L"FolderNote-IconTests-") + id);
    auto custom = root / L"自定义😀", plain = root / L"普通目录";
    std::filesystem::create_directories(custom); std::filesystem::create_directories(plain);
    auto iconFile = root / L"folder.ico";
    std::filesystem::copy_file(std::filesystem::path(FOLDERNOTE_ICON_PATH), iconFile);
    std::wstring ini = L"[.ShellClassInfo]\r\nIconResource=" + iconFile.native() + L",0\r\n";
    {
      std::ofstream file(custom / L"desktop.ini", std::ios::binary);
      wchar_t bom = 0xfeff; file.write(reinterpret_cast<char *>(&bom), sizeof(bom));
      file.write(reinterpret_cast<const char *>(ini.data()), ini.size() * sizeof(wchar_t));
    }
    check(PathMakeSystemFolderW(custom.c_str()) != FALSE, "custom folder attributes");
    check(SetFileAttributesW((custom / L"desktop.ini").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) != FALSE, "ini attributes");
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, custom.c_str(), nullptr);
    auto normal = read(Source::file(plain.native()), 16);
    auto personalized = read(Source::file(custom.native()), 16);
    check(normal && personalized, "load actual folder icons");
    check(pixels(normal->handle) != pixels(personalized->handle), "desktop.ini icon must differ from generic folder");
    Microsoft::WRL::ComPtr<IShellItem> item;
    Microsoft::WRL::ComPtr<IExtractIconW> extractor;
    check(SUCCEEDED(SHCreateItemFromParsingName(custom.c_str(), nullptr, IID_PPV_ARGS(&item))), "bind custom shell item");
    check(SUCCEEDED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&extractor))), "bind custom icon provider");
    wchar_t location[32768]{}; int iconIndex = 0; UINT flags = 0;
    check(SUCCEEDED(extractor->GetIconLocation(GIL_FORSHELL, location, ARRAYSIZE(location), &iconIndex, &flags)), "read custom icon source");
    std::wcout << L"Shell icon location: " << location << L", " << iconIndex << L", flags " << flags << L'\n';
    check(!_wcsicmp(location, iconFile.c_str()), "Shell must use the folder's own icon source");
    // Shell may resample/composite ICO data differently from LoadImage. Verify
    // the actual icon provider's source and distinguish it from a generic folder.
    check(read(Source::system(SIID_SETTINGS), 16) != nullptr, "Windows settings icon");
    check(read(Source::folder(FOLDERID_Downloads), 24) != nullptr, "Windows downloads icon at scaled size");
    {
      Cache cache(nullptr, 0, 16);
      Image loaded;
      for (int i = 0; i < 400 && !loaded; ++i) { loaded = cache.get(Source::file(custom.native())); if (!loaded) Sleep(10); }
      check(loaded && pixels(loaded->handle) == pixels(personalized->handle), "async cache preserves custom icon");
    }
    {
      auto gate = std::make_shared<Gate>();
      auto loader = [gate](const Source &, int) -> Image {
        std::unique_lock lock(gate->mutex); ++gate->started; gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
        ++gate->finished; gate->changed.notify_all(); return {};
      };
      auto cache = std::make_unique<Cache>(nullptr, 0, 16, loader);
      check(!cache->get(Source::file(L"C:\\slow")), "uncached requests return immediately");
      for (int i = 0; i < 20; ++i) cache->get(Source::file(L"c:\\SLOW"));
      cache->get(Source::file(L"C:\\second"));
      {
        std::unique_lock lock(gate->mutex);
        check(gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->started == 2; }), "deduplicated requests use at most two workers");
      }
      cache->get(Source::file(L"C:\\queued"));
      cache.reset(); // Must return even though both shell icon readers are blocked.
      {
        std::lock_guard lock(gate->mutex); check(gate->finished == 0, "menu closes without waiting for blocked extraction");
        gate->release = true;
      }
      gate->changed.notify_all();
      {
        std::unique_lock lock(gate->mutex);
        check(gate->changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate->finished == 2; }), "workers can finish safely after owner destruction");
        check(gate->started == 2, "closing cancels queued icons");
      }
    }
    std::cout << "PASS " << checks << " shell icon checks: custom icon provider/source, system icons, async delivery, deduplication, bounded workers, cancellation\n";
    std::wcout << L"Test fixture: " << root.native() << L'\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; CoUninitialize(); return 1; }
  CoUninitialize(); return 0;
}
