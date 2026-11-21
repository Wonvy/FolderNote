#include "../src/quick_find.h"
#include "../src/quick_find_controls.h"
#include <commctrl.h>
#include <objbase.h>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    using namespace quick_find;
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
      const auto dpi = GetDpiForWindow(parent); const int size = MulDiv(16, dpi, 96), x = MulDiv(10, dpi, 96), height = MulDiv(32, dpi, 96);
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
