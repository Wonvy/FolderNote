#include "../src/quick_find.h"
#include "../src/quick_find_controls.h"
#include "quick_find_test_settings.h"
#include <commctrl.h>
#include <objbase.h>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <fstream>

static void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
// Optional native rendering artifacts, from hidden fixture windows only.
static void render(HWND window, const std::filesystem::path &path) {
  RECT rect{}; GetClientRect(window, &rect);
  BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
  info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
  const auto dc = CreateCompatibleDC(nullptr);
  void *pixels = nullptr;
  const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  check(bitmap && pixels, "allocate preview bitmap");
  const auto old = SelectObject(dc, bitmap);
  SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_CHILDREN);
  GdiFlush();
  BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
  const DWORD bytes = rect.right * rect.bottom * 4; file.bfSize = file.bfOffBits + bytes;
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(&file), sizeof(file));
  output.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(info.bmiHeader));
  output.write(static_cast<const char *>(pixels), bytes);
  SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
  check(output.good(), "write native popup preview");
}
int main(int argc, char **argv) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    using namespace quick_find;
    QuickFindTestSettings isolated;
    check(settings() == Settings{}, "missing preferences preserve defaults");
    DWORD legacy = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"PinyinQuickFind", REG_DWORD, &legacy, sizeof(legacy));
    check(settings().explorer && settings().dialogs, "migrate existing external search switch");
    Settings preferences;
    preferences.boxes = preferences.lists = preferences.openFolders = preferences.atPointer = preferences.showIcons = false;
    preferences.explorer = preferences.openFiles = true; preferences.visibleRows = 12;
    check(saveSettings(preferences) == ERROR_SUCCESS && settings() == preferences, "independent settings persist together");
    check(!settings().opens(true) && settings().opens(false) && !settings().opens(false, true), "file and folder actions differ; picker files never submit");
    auto invalid = preferences; invalid.visibleRows = 0;
    check(saveSettings(invalid) == ERROR_INVALID_PARAMETER && settings() == preferences, "invalid row count preserves saved settings");
    DWORD damaged = 0xffffffff;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"QuickFindOptions", REG_DWORD, &damaged, sizeof(damaged));
    check(settings().visibleRows == 9 && settings().explorer && !settings().openFiles, "corrupt settings use safe legacy defaults");
    check(saveSettings({}) == ERROR_SUCCESS, "restore test defaults");
    check(matches(L"设计素材", L"sjsc"), "Chinese initials");
    check(matches(L"设计素材", L"SJSC"), "case insensitive");
    check(matches(L"設計素材", L"sjsc"), "traditional Chinese");
    check(matches(L"重庆项目", L"cqxm"), "polyphonic chong");
    check(matches(L"重复文件", L"cfwj"), "polyphonic other word");
    check(matches(L"银行账单", L"yhzd"), "polyphonic hang");
    check(matches(L"行程安排", L"xcap"), "polyphonic xing");
    check(matches(L"PS设计_2026.png", L"pssj2026"), "mixed letters, Chinese and digits");
    check(matches(L"【工作】设计素材", L"sjsc"), "initial substring");
    check(matches(L"设计素材", L"设计"), "literal Chinese");
    check(matches(L"素材设计", L"sc设计"), "mixed literal Chinese and initials");
    check(matches(L"C:\\Projects\\设计素材", L"sjsc"), "folder list full paths");
    check(matches(L"ＡＢＣ设计", L"abcsj"), "full-width letters");
    check(matches(L"𠀀设计", L"hsj"), "supplementary Unicode character");
    check(!matches(L"设计素材", L"sucai"), "initials are not full pinyin");
    check(!matches(L"素材", L"sz"), "no unrelated match");
    check(matches(L"设计素材", L""), "empty filter keeps rows");
    check(Name(L"sjsc.txt").rank(L"sj") < Name(L"设计素材").rank(L"sj"), "literal prefix wins");
    check(Name(L"素材").rank(L"sc") < Name(L"ScriptData").rank(L"sc"), "complete initials precede English prefix");
    check(Name(L"sc").rank(L"sc") < Name(L"素材").rank(L"sc"), "complete literal name still wins");
    {
      const std::vector<Name> mixed{ Name(L"ScriptData"), Name(L"scriptprism.exe"), Name(L"step"), Name(L"素材"), Name(L"素材库") };
      Session find;
      auto row = find.type(L's', 1000, mixed, -1);
      check(row == 0, "partial English prefix remains available");
      row = find.type(L'c', 1250, mixed, row);
      check(row == 3, "sc leaves selected ScriptData for complete Chinese initials");
      check(find.next(mixed, row) == 4, "other initial matches remain reachable");
      row = find.type(L'\b', 1300, mixed, row);
      check(row == 0, "backspace restores partial English match");
      for (auto letter : std::wstring(L"cript")) row = find.type(letter, 1400, mixed, row);
      check(row == 0, "longer English query selects ScriptData");
    }
    std::vector<Name> names{ Name(L"设计素材"), Name(L"视频素材"), Name(L"截图"), Name(L"测试") };
    Session session;
    int row = session.type(L's', 1000, names, -1);
    check(row == 0, "initial letter selects first match");
    row = session.type(L'j', 1100, names, row);
    check(row == 0 && session.query() == L"sj", "incremental query");
    check(session.type(L'\b', 1200, names, row) == 0 && session.query() == L"s", "backspace");
    check(session.type(L's', 1250, names, row) == 1 && session.query() == L"s", "repeated letter cycles");
    check(session.next(names, 1) == 3, "next match includes substring");
    check(session.next(names, 3) == 0, "next match wraps");
    check(session.next(names, 0, true) == 3, "previous match wraps");
    check(session.type(L'c', 4000, names, 0) == 3 && session.query() == L"c", "timeout starts fresh query");
    check(session.type(L'x', 4100, names, 3) == -1 && session.query() == L"cx", "no match preserves query");
    session.clear(); check(!session.active(4200), "clear query");
    check(session.type(L'a', 4300, {}, -1) == -1, "empty list");
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&common);
    const auto parent = CreateWindowExW(0, L"STATIC", L"QuickFindTests", WS_POPUP, -20000, -20000, 300, 200,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(parent != nullptr, "create control fixture");
    if (argc == 3 && std::string_view(argv[1]) == "--render-preview") {
      const auto directory = std::filesystem::path(argv[2]); std::filesystem::create_directories(directory);
      std::vector<std::wstring> samples{L"封面设计.pdf", L"方案素材.pdf", L"项目分析报告.pdf", L"产品发布清单.pdf"};
      for (int i = 1; i <= 8; ++i) samples.push_back(L"封装参数-" + std::to_wstring(i) + L".pdf");
      const auto icon = quick::icons::read(quick::icons::Source::system(SIID_DOCNOASSOC), 16);
      std::vector<CandidateIcon> icons(samples.size(), CandidateIcon::copy(icon ? icon->handle : nullptr));
      for (DWORD mode : {1u, 2u}) {
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"Theme", REG_DWORD, &mode, sizeof(mode));
        for (const auto &entry : {std::pair{L"f", "overflow"}, std::pair{L"项目分析", "single"}, std::pair{L"zzz", "empty"}}) {
          Popup preview; preview.begin(parent, samples, {}, icons);
          for (auto character : std::wstring(entry.first)) preview.character(character);
          render(preview.handle(), directory / (std::string(mode == 1 ? "light-" : "dark-") + entry.second + ".bmp"));
        }
      }
      RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\ShibaKit", L"Theme");
    }
    const auto list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | LBS_HASSTRINGS | LBS_NOTIFY,
                                      0, 0, 220, 160, parent, reinterpret_cast<HMENU>(1), nullptr, nullptr);
    for (auto name : {L"视频素材", L"设计素材", L"重庆项目"}) SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
    attach(list); attach(list); SetFocus(list);
    check(GetFocus() == list, "focus fixture");
    SendMessageW(list, WM_CHAR, L's', 0); SendMessageW(list, WM_CHAR, L'j', 0);
    check(SendMessageW(list, LB_GETCURSEL, 0, 0) == -1, "typing lists candidates without selecting original row");
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    check(SendMessageW(list, LB_GETCURSEL, 0, 0) == 1, "native list selects Chinese row");
    SendMessageW(list, WM_KEYDOWN, VK_ESCAPE, 0);
    SendMessageW(list, WM_CHAR, L'c', 0); SendMessageW(list, WM_CHAR, L'q', 0);
    SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
    check(SendMessageW(list, LB_GETCURSEL, 0, 0) == 2, "Esc starts another query");
    check(SendMessageW(list, LB_GETCOUNT, 0, 0) == 3, "locating never filters rows");
    {
      auto disabled = settings(); disabled.lists = false; saveSettings(disabled);
      SendMessageW(list, LB_SETCURSEL, WPARAM(-1), 0);
      SendMessageW(list, WM_CHAR, L's', 0); SendMessageW(list, WM_CHAR, L'j', 0); SendMessageW(list, WM_KEYDOWN, VK_RETURN, 0);
      check(SendMessageW(list, LB_GETCURSEL, 0, 0) == -1, "disabled internal search keeps native input");
      saveSettings({});
      {
        Popup sourced;
        int selected = -1;
        sourced.begin(parent, {L"ScriptData", L"素材  ·  工作 / 收藏"}, [&](int row) { selected = row; }, {}, {L"ScriptData", L"素材"});
        sourced.character(L's'); sourced.character(L'c'); sourced.key(VK_RETURN);
        check(selected == 1, "source labels do not alter exact-initial ranking or candidate identity");
      }
      std::vector<std::wstring> many;
      for (int i = 0; i < 20; ++i) many.push_back(L"sample" + std::to_wstring(i));
      Popup popup;
      popup.begin(parent, many, {}); popup.character(L's');
      RECT nine{}; GetWindowRect(popup.handle(), &nine);
      auto compact = settings(); compact.visibleRows = 5; compact.showIcons = false; compact.atPointer = false;
      saveSettings(compact);
      popup.begin(parent, many, {}); popup.character(L's');
      RECT five{}; GetWindowRect(popup.handle(), &five);
      const int itemHeight = static_cast<int>(SendDlgItemMessageW(popup.handle(), 2, LB_GETITEMHEIGHT, 0, 0));
      check(nine.bottom - nine.top - (five.bottom - five.top) == 4 * itemHeight, "visible row preference changes popup height");
      check(SendDlgItemMessageW(popup.handle(), 2, LB_GETCOUNT, 0, 0) == 20, "visible count never truncates matches");
      const auto results = GetDlgItem(popup.handle(), 2), bar = GetDlgItem(popup.handle(), 4);
      const auto visible = [](HWND child) { return (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0; };
      check(!(GetWindowLongPtrW(results, GWL_STYLE) & (WS_VSCROLL | WS_HSCROLL)), "no classic scrollbar in candidates");
      check(visible(bar), "slim scrollbar appears for overflow");
      check(SendMessageW(results, LB_GETTOPINDEX, 0, 0) == 0 && SendMessageW(results, LB_GETCURSEL, 0, 0) == 0,
            "resizing overflow popup keeps the best match selected and visible");
      popup.key(VK_NEXT);
      check(SendDlgItemMessageW(popup.handle(), 2, LB_GETCURSEL, 0, 0) == 5, "Page Down respects configured page size");
      popup.key(VK_END);
      SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(bar, SB_CTL, &info);
      check(info.nPos == 15 && info.nPage == 5 && info.nMax == 19 && SendMessageW(results, LB_GETTOPINDEX, 0, 0) == info.nPos,
            "keyboard selection synchronizes the custom scrollbar with the real list");
      SendMessageW(popup.handle(), WM_VSCROLL, SB_TOP, reinterpret_cast<LPARAM>(bar));
      check(SendMessageW(results, LB_GETTOPINDEX, 0, 0) == 0, "accessible scrollbar command moves actual rows");
      SendMessageW(popup.handle(), WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), 0);
      GetScrollInfo(bar, SB_CTL, &info);
      UINT wheelLines = 0; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
      check(info.nPos == (wheelLines == WHEEL_PAGESCROLL ? 5 : static_cast<int>(std::min(15u, wheelLines))) &&
            SendMessageW(results, LB_GETTOPINDEX, 0, 0) == info.nPos, "wheel respects system setting and updates both list and thumb");
      SCROLLBARINFO barInfo{sizeof(barInfo)}; SendMessageW(bar, SBM_GETSCROLLBARINFO, 0, reinterpret_cast<LPARAM>(&barInfo));
      RECT barRect{}; GetClientRect(bar, &barRect);
      const int grabY = (barInfo.xyThumbTop + barInfo.xyThumbBottom) / 2;
      const auto originalFocus = GetFocus();
      SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(barRect.right / 2, grabY));
      SendMessageW(bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(barRect.right / 2, barRect.bottom + 50));
      SendMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(barRect.right / 2, barRect.bottom + 50));
      check(SendMessageW(results, LB_GETTOPINDEX, 0, 0) == 15 && GetFocus() == originalFocus, "drag thumb to last page without stealing original focus");
      for (auto letter : std::wstring(L"ample19")) popup.character(letter);
      RECT one{}; GetWindowRect(popup.handle(), &one);
      check(SendMessageW(results, LB_GETCOUNT, 0, 0) == 1 && !visible(bar) && visible(results) &&
            one.bottom - one.top < five.bottom - five.top, "single result shrinks height and hides scrollbar");
      popup.character(L'z');
      RECT empty{}; GetWindowRect(popup.handle(), &empty);
      wchar_t emptyLabel[64]{}; GetWindowTextW(GetDlgItem(popup.handle(), 3), emptyLabel, 64);
      check(!visible(results) && !visible(bar) && empty.bottom - empty.top < one.bottom - one.top && std::wstring(emptyLabel) == L"无匹配",
            "empty search collapses to header without blank list or scrollbar");
      popup.key(VK_BACK); popup.key(VK_BACK);
      check(SendMessageW(results, LB_GETCOUNT, 0, 0) == 11 && visible(results) && visible(bar), "backspace restores overflow list and scrollbar");
      SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(barRect.right / 2, 5));
      popup.close();
      check(GetCapture() != bar && !popup.active(), "closing popup releases an ongoing thumb drag");
      saveSettings({});
    }
    const auto edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD, 0, 0, 100, 20, parent, nullptr, nullptr, nullptr);
    attach(edit); SetFocus(edit); SendMessageW(edit, WM_CHAR, L's', 0);
    wchar_t text[8]{}; GetWindowTextW(edit, text, 8);
    check(std::wstring(text) == L"s", "edit boxes retain normal typing");
    {
      // Render the production owner-drawn row, after ranking moved 素材 ahead
      // of ScriptData. Its native icon must follow the original item index.
      auto folder = quick::icons::read(quick::icons::Source::system(SIID_FOLDER), 16);
      auto shield = quick::icons::read(quick::icons::Source::system(SIID_SHIELD), 16);
      check(folder && shield, "load native candidate icons");
      Popup popup;
      popup.begin(parent, {L"ScriptData", L"素材"}, {}, {CandidateIcon::copy(folder->handle), CandidateIcon::copy(shield->handle)});
      popup.character(L's'); popup.character(L'c');
      HDC actual = CreateCompatibleDC(nullptr), expected = CreateCompatibleDC(nullptr);
      BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 256;
      info.bmiHeader.biHeight = -256; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
      void *bits = nullptr;
      auto a = CreateDIBSection(actual, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      auto b = CreateDIBSection(expected, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      auto oldA = SelectObject(actual, a), oldB = SelectObject(expected, b);
      const auto dpi = GetDpiForWindow(parent); const int size = MulDiv(16, dpi, 96), x = MulDiv(10, dpi, 96),
          height = static_cast<int>(SendDlgItemMessageW(popup.handle(), 2, LB_GETITEMHEIGHT, 0, 0));
      DRAWITEMSTRUCT item{}; item.CtlType = ODT_LISTBOX; item.CtlID = 2; item.itemID = 0;
      item.hwndItem = GetDlgItem(popup.handle(), 2); item.hDC = actual; item.rcItem = {0, 0, 256, height};
      SendMessageW(popup.handle(), WM_DRAWITEM, 2, reinterpret_cast<LPARAM>(&item));
      auto background = CreateSolidBrush(ui::palette(ui::configuredTheme()).field);
      FillRect(expected, &item.rcItem, background); DeleteObject(background);
      DrawIconEx(expected, x, (height - size) / 2, shield->handle, size, size, 0, nullptr, DI_NORMAL);
      bool same = true;
      for (int y = (height - size) / 2; y < (height + size) / 2; ++y)
        for (int column = x; column < x + size; ++column) same &= GetPixel(actual, column, y) == GetPixel(expected, column, y);
      SelectObject(actual, oldA); SelectObject(expected, oldB); DeleteObject(a); DeleteObject(b); DeleteDC(actual); DeleteDC(expected);
      check(same, "ranked candidate paints the original native icon without cropping");
    }
    DestroyWindow(parent);
    CoUninitialize();
    std::cout << "Chinese matching, query state, native list selection and edit isolation passed.\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
