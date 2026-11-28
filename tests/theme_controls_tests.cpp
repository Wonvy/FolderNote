// Hidden test-owned windows only; no real user input or settings changes.
#include "../src/quick_manage.cpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
namespace {
int checks = 0;
void check(bool value, const char *why) { ++checks; if (!value) throw std::runtime_error(why); }
struct Registry {
  HKEY real = nullptr, key = nullptr; std::wstring name;
  Registry() {
    GUID guid{}; CoCreateGuid(&guid); wchar_t id[40]{}; StringFromGUID2(guid, id, 40);
    name = std::wstring(L"Software\\ShibaKitThemeTests\\") + id;
    check(!RegOpenCurrentUser(KEY_ALL_ACCESS, &real) &&
        !RegCreateKeyExW(real, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) &&
        !RegOverridePredefKey(HKEY_CURRENT_USER, key), "isolate test settings");
  }
  ~Registry() { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(key); RegDeleteTreeW(real, name.c_str()); RegCloseKey(real); }
};
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    auto instance = GetModuleHandleW(nullptr);
    HWND parent = CreateWindowW(L"STATIC", L"test", WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, instance, nullptr);
    check(parent != nullptr, "hidden test parent");
    for (auto mode : {ui::ThemeMode::Light, ui::ThemeMode::Dark}) for (UINT dpi : {96u, 144u, 192u}) {
      ui::Theme theme; theme.reset(mode); ui::ScrollBar bar; int callback = -1;
      bar.create(parent, 1, theme, [&](int value) { callback = value; });
      bar.move(0, 0, MulDiv(12, dpi, 96), MulDiv(240, dpi, 96)); bar.configure(100, 20, 0, dpi);
      check((GetWindowLongPtrW(bar.window(), GWL_STYLE) & WS_VISIBLE) != 0 && bar.maximum() == 80, "show overflow thumb");
      auto thumb = bar.thumb(); check(thumb.bottom > thumb.top && thumb.right > thumb.left, "scaled thumb geometry");
      SendMessageW(bar.window(), WM_KEYDOWN, VK_END, 0); check(bar.position() == 80 && callback == 80, "keyboard end");
      SendMessageW(bar.window(), WM_KEYDOWN, VK_PRIOR, 0); check(bar.position() == 60, "keyboard page up");
      bar.scrollTo(-100); check(bar.position() == 0 && callback == 0, "clamp negative position");
      thumb = bar.thumb(); int x = (thumb.left + thumb.right) / 2, y = (thumb.top + thumb.bottom) / 2;
      SendMessageW(bar.window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
      SendMessageW(bar.window(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, MulDiv(1000, dpi, 96)));
      SendMessageW(bar.window(), WM_LBUTTONUP, 0, MAKELPARAM(x, MulDiv(1000, dpi, 96)));
      check(bar.position() == 80 && GetCapture() != bar.window(), "drag beyond track clamps and releases capture");
      SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(bar.window(), SB_CTL, &info);
      check(info.nPos == 80 && info.nMax == 99 && info.nPage == 20, "native range remains available");
      bar.scrollTo(0); bar.wheel(-60); check(bar.position() == 0, "accumulate partial wheel delta");
      bar.wheel(-60);
      UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
      check(bar.position() == (lines == WHEEL_PAGESCROLL ? 20 : std::min<int>(80, lines)), "respect system wheel amount");
      bar.configure(2, 20, 100, dpi);
      check(bar.position() == 0 && !(GetWindowLongPtrW(bar.window(), GWL_STYLE) & WS_VISIBLE), "hide unnecessary scrollbar after range shrinks");
    }
    {
      BITMAPINFO bmi{};
      bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), 300, -40, 1, 32, BI_RGB};
      void *pixels = nullptr;
      HDC dc = CreateCompatibleDC(nullptr);
      HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
      HGDIOBJ old = SelectObject(dc, dib);
      auto *raw = static_cast<DWORD *>(pixels);
      std::fill_n(raw, 300 * 40, 0x00112233u);
      const int saved = SaveDC(dc);
      IntersectClipRect(dc, 0, 0, 30, 24);
      RECT parentArea{-260, -6, 40, 34};
      SetDCBrushColor(dc, RGB(240, 240, 240));
      FillRect(dc, &parentArea, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      RestoreDC(dc, saved);
      check(raw[10 * 300 + 20] != 0x00112233u && raw[10 * 300 + 100] == 0x00112233u,
            "clipping child CS_PARENTDC background fills preserves title bar pixels outside the button");
      SelectObject(dc, old);
      DeleteObject(dib);
      DeleteDC(dc);
    }
    DestroyWindow(parent);
    {
      Registry isolated;
      for (int i = 0; i < 20; ++i) quick::visit(L"C:\\ThemeTest\\目录😀" + std::to_wstring(i));
      quick::pin(L"C:\\ThemeTest\\置顶", true);
      HWND owner = CreateWindowW(L"STATIC", L"test settings host", WS_OVERLAPPED, 0, 0, 1000, 600, nullptr, nullptr, instance, nullptr);
      HWND window = quick::createManagerPage(instance, owner, ui::ThemeMode::Dark);
      check(window && GetParent(window) == owner && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD), "manager is a child of the unified settings window");
      auto &state = *reinterpret_cast<quick::Manager *>(GetWindowLongPtrW(window, GWLP_USERDATA));
      MoveWindow(window, 200, 80, 700, 400, FALSE);
      check(window && state.history && state.pins, "create native folder manager");
      check(!GetDlgItem(window, IDOK) && !GetDlgItem(window, IDCANCEL), "embedded manager has no independent close buttons");
      check(!(GetWindowLongPtrW(state.history, GWL_STYLE) & (WS_HSCROLL | WS_VSCROLL)), "remove classic list scrollbars");
      check(state.historyScroll.maximum() > 0 && state.pinsScroll.maximum() == 0, "only overflowing list gets a scrollbar");
      state.historyScroll.scrollTo(state.historyScroll.maximum());
      check(SendMessageW(state.history, LB_GETTOPINDEX, 0, 0) == state.historyScroll.position(), "thumb scrolls actual list");
      SendMessageW(state.history, LB_SETCURSEL, 0, 0);
      check(SendMessageW(state.history, LB_GETTOPINDEX, 0, 0) == state.historyScroll.position(), "list selection synchronizes thumb");
      wchar_t label[256]{}; SendMessageW(state.history, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(label));
      check(state.data.history.front() == label, "native accessibility retains full Unicode path");
      std::atomic<bool> actualPath{false}; auto expected = state.data.history.front();
      state.icons = std::make_unique<quick::icons::Cache>(window, quick::iconsReady, 16,
          [&](const quick::icons::Source &source, int) -> quick::icons::Image {
            actualPath = source.kind == quick::icons::Source::Kind::File && source.path == expected; return {};
          });
      HDC dc = CreateCompatibleDC(nullptr); HBITMAP bitmap = CreateBitmap(320, 30, 1, 32, nullptr); auto old = SelectObject(dc, bitmap);
      DRAWITEMSTRUCT item{}; item.CtlType = ODT_LISTBOX; item.hwndItem = state.history; item.hDC = dc; item.rcItem = {0, 0, 320, 30};
      state.draw(item);
      for (int i = 0; i < 100 && !actualPath.load(); ++i) Sleep(5);
      check(actualPath, "manager asks Shell for the actual folder icon");
      SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
      quick::setManagerTheme(window, ui::ThemeMode::Light);
      check(state.theme.mode == ui::ThemeMode::Light && !state.theme.colors.dark, "child page previews host theme");
      for (UINT pageDpi : {96u, 144u, 192u}) {
        SendMessageW(window, WM_DPICHANGED, MAKELONG(pageDpi, pageDpi), 0);
        const int width = MulDiv(650, pageDpi, 96), height = MulDiv(300, pageDpi, 96);
        MoveWindow(window, 0, 0, width, height, FALSE);
        RECT left{}, right{}, button{}; GetWindowRect(state.pins, &left); GetWindowRect(state.history, &right); GetWindowRect(GetDlgItem(window, 25), &button);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&left), 2);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&right), 2);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&button), 2);
        check(left.right < right.left && right.right <= width && left.bottom <= button.top && button.bottom <= height,
              "resized embedded columns and actions remain inside the page at scaled DPI");
        check(SendMessageW(state.history, LB_GETITEMHEIGHT, 0, 0) == MulDiv(30, pageDpi, 96), "embedded list rows follow host DPI");
      }
      quick::pin(L"C:\\ThemeTest\\NewPin", true); quick::refreshManagerPage(window);
      check(state.data.pins.size() == 2 && SendMessageW(state.pins, LB_GETCOUNT, 0, 0) == 2, "switching to page reloads current folder data");
      SendMessageW(state.pins, LB_SETCURSEL, 0, 0);
      SendMessageW(window, WM_COMMAND, MAKEWPARAM(23, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 23)));
      check(quick::load().pins[1] == L"C:\\ThemeTest\\置顶", "embedded move command saves immediately");
      HWND list = state.history, scroll = state.historyScroll.window();
      DestroyWindow(window);
      check(!IsWindow(list) && !IsWindow(scroll) && IsWindow(owner), "closing child releases controls without closing settings host");
      MSG quit{}; check(!PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE), "child lifetime never exits host message loop");
      DestroyWindow(owner);
    }
    std::cout << "PASS " << checks << " theme controls: scaled drag, wheel, keyboard, range, native list scrolling, actual folder icons\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; CoUninitialize(); return 1; }
  CoUninitialize(); return 0;
}
