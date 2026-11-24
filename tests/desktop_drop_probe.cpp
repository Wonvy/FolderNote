// Interactive OLE regression fixture. Drag the generated file/folder onto the
// native rim or the real WinUI box content. Does not move any user files.
#include "../src/desktop_groups_winui.h"
#include "../src/desktop_groups_preview.h"
#include "../src/desktop_groups_zorder.h"
#include "../src/desktop_groups_glass.h"
#include "../src/quick_find.h"
#include "../src/quick_find_popup.h"
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/implements.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

using Microsoft::WRL::ComPtr;
namespace {
using Classic = Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>;
std::filesystem::path root;
HWND statusWindow = nullptr;
int drops = 0;
int liveTargets = 0;

void log(const char *message) { std::cout << message << std::endl; std::ofstream(root / L"trace.txt", std::ios::app) << message << '\n'; }
// Diagnostic output only contains ASCII test input; replace anything else.
std::string ascii(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (const wchar_t c : text) result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return result;
}
class Source final : public Microsoft::WRL::RuntimeClass<Classic, IDropSource> {
public:
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL cancel, DWORD keys) override {
    return cancel ? DRAGDROP_S_CANCEL : (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};
class Target final : public Microsoft::WRL::RuntimeClass<Classic, IDropTarget> {
public:
  Target() { ++liveTargets; }
  ~Target() { --liveTargets; }
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *, DWORD, POINTL, DWORD *effect) override {
    log("DragEnter"); *effect &= DROPEFFECT_COPY; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *effect) override {
    *effect &= DROPEFFECT_COPY; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override { log("DragLeave"); return S_OK; }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL, DWORD *effect) override {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (SUCCEEDED(data->GetData(&format, &medium))) {
      const auto count = DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0xffffffff, nullptr, 0);
      ReleaseStgMedium(&medium);
      std::cout << "Drop files=" << count << std::endl;
      if (count == 2) ++drops;
    }
    SetWindowTextW(statusWindow, (L"Successful drops: " + std::to_wstring(drops)).c_str());
    *effect &= DROPEFFECT_COPY; return S_OK;
  }
};
ComPtr<IDataObject> testData() {
  PIDLIST_ABSOLUTE parent = ILCreateFromPathW(root.c_str());
  PIDLIST_ABSOLUTE file = ILCreateFromPathW((root / L"sample.txt").c_str());
  PIDLIST_ABSOLUTE folder = ILCreateFromPathW((root / L"sample folder").c_str());
  PCUITEMID_CHILD children[]{ILFindLastID(file), ILFindLastID(folder)};
  ComPtr<IDataObject> data;
  SHCreateDataObject(parent, 2, children, nullptr, IID_PPV_ARGS(&data));
  ILFree(parent); ILFree(file); ILFree(folder);
  return data;
}
void drag() {
  log("begin drag");
  if (auto data = testData()) {
    auto source = Microsoft::WRL::Make<Source>();
    DWORD effect = 0;
    const auto drop = DoDragDrop(data.Get(), source.Get(), DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    std::cout << "DoDragDrop hr=" << std::hex << drop << " effect=" << effect << std::dec << std::endl;
    std::ofstream(root / L"trace.txt", std::ios::app) << "result=" << std::hex << drop << " effect=" << effect << '\n';
  }
}
LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_LBUTTONDOWN) {
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    std::ofstream(root / L"trace.txt", std::ios::app) << "down=" << point.x << ',' << point.y << '\n';
    if (point.x < 280 && point.y < 240 && DragDetect(window, point)) drag();
    return 0;
  }
  if (message == WM_PAINT) {
    PAINTSTRUCT ps{}; const auto dc = BeginPaint(window, &ps);
    RECT text{25, 75, 260, 240};
    DrawTextW(dc, L"Drag from here\n\nTemporary file + folder", -1, &text, DT_WORDBREAK);
    EndPaint(window, &ps); return 0;
  }
  if (message == WM_CLOSE) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(window, message, w, l);
}
}
int main(int argc, char **argv) {
  const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
  const bool desktopHost = mode == "--verify-desktop";
  const bool verify = mode == "--verify" || mode == "--verify-parent-only" || desktopHost;
  const bool parentOnly = mode == "--parent-only" || mode == "--verify-parent-only";
  const bool preview = mode == "--preview" || mode == "--preview-list";
  const bool findTest = mode == "--verify-find" || desktopHost;
  int result = 0;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  if (FAILED(OleInitialize(nullptr))) return 1;
  root = std::filesystem::temp_directory_path() / (L"FolderNoteDropProbe-" + std::to_wstring(GetCurrentProcessId()));
  std::filesystem::create_directories(root / L"sample folder");
  std::ofstream(root / L"sample.txt") << "temporary drag fixture";
  {
    desktop_groups::winui::Runtime runtime;
    if (!runtime.available()) { log("WinUI unavailable"); return 77; }
    WNDCLASSW cls{}; cls.lpfnWndProc = proc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"FolderNote.DropProbe"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    const auto desktop = desktopHost ? desktop_groups::desktopWindowHost() : nullptr;
    if (desktopHost && !desktop) { log("Shell desktop unavailable"); return 77; }
    desktop_groups::DesktopDpiScope hostDpi(desktop);
    HWND window = CreateWindowExW(preview || desktopHost ? WS_EX_NOACTIVATE : 0, cls.lpszClassName, L"FolderNote Drop Probe",
        (desktopHost ? WS_POPUP : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN,
        100, 100, 900, 600, nullptr, nullptr, cls.hInstance, nullptr);
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        300, 30, 550, 470, window, nullptr, cls.hInstance, nullptr);
    statusWindow = CreateWindowExW(0, L"STATIC", L"Successful drops: 0", WS_CHILD | WS_VISIBLE,
        25, 300, 260, 60, window, nullptr, cls.hInstance, nullptr);
    auto target = Microsoft::WRL::Make<Target>();
    desktop_groups::DesktopGlass glass;
    std::cout << "Register native hr=" << std::hex << RegisterDragDrop(host, target.Get()) << std::dec << std::endl;
    {
      desktop_groups::winui::Events events;
      quick_find::Popup query;
      std::vector<std::wstring> findNames{L"设计素材", L"重庆项目"};
      desktop_groups::winui::Surface *findSurface = nullptr;
      int found = -1, opened = 0;
      std::wstring characters;
      if (findTest) events.findInput = [&](UINT message, WPARAM key) {
        if (message == WM_KILLFOCUS) query.close();
        if (message == WM_KEYDOWN && query.key(static_cast<UINT>(key))) return true;
        if (message != WM_CHAR || !quick_find::inputCharacter(static_cast<wchar_t>(key))) return false;
        characters += static_cast<wchar_t>(key);
        if (!query.active()) query.begin(host, findNames, [&](int row) {
          found = row;
          if (findSurface) { findSurface->select({found}); findSurface->reveal(found); }
        });
        query.character(static_cast<wchar_t>(key));
        return true;
      };
      if (findTest) events.open = [&](int) { ++opened; };
      if (preview) events.previewAvailable = [] { return desktop_groups::desktopPreviewAvailable(); };
      if (preview) events.preview = [&](int row) {
        const bool handled = row >= 0 && row <= 1 && desktop_groups::previewDesktopItem(
            (root / (row == 0 ? L"sample.txt" : L"sample folder")).native());
        std::ofstream(root / L"trace.txt", std::ios::app) << "preview row=" << row << '\n';
        log(handled ? "Preview delivered" : "Preview unavailable");
        return handled;
      };
      desktop_groups::winui::Surface surface(host, std::move(events), parentOnly ? nullptr : target.Get());
      findSurface = &surface;
      if (!surface.available()) { log("Surface unavailable"); return 3; }
      if (desktopHost && !desktop_groups::attachDesktopWindow(window, desktop)) {
        log("Desktop attachment failed"); return 10;
      }
      surface.place({10, 10, 540, 460});
      surface.appearance(false, false, RGB(235, 240, 245), desktopHost ? 128 : 255, desktopHost);
      surface.header({L"WinUI drop target"}, 0, false, false, false, 8,
          RGB(0,0,0), RGB(80,80,80), RGB(120,120,120), RGB(0,120,200), 36,
          desktop_groups::TitleStyle::Text, nullptr);
      std::vector<desktop_groups::winui::Item> items{{preview ? L"sample.txt" : L"Existing item", (root / L"sample.txt").native(), nullptr}};
      if (preview) items.push_back({L"sample folder", (root / L"sample folder").native(), nullptr});
      if (findTest) items = {{L"设计素材", L"", nullptr}, {L"重庆项目", L"", nullptr}};
      surface.rows(items,
                   mode == "--preview-list", 32);
      if (preview) surface.select({0});
      surface.show(true);
      if (findTest) {
        ShowWindow(window, SW_SHOWNORMAL);
        const auto pump = [&](DWORD duration) {
          const auto end = GetTickCount64() + duration;
          while (GetTickCount64() < end) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
              if (runtime.preTranslate(message)) continue;
              TranslateMessage(&message); DispatchMessageW(&message);
            }
            Sleep(10);
          }
        };
        pump(150);
        if (desktopHost) {
          for (int i = 0; i < 100 && !surface.acrylicActive(); ++i) pump(25);
          if (!surface.acrylicActive()) { log("FAIL: desktop glass background did not load"); result = 12; }
          RECT bounds{}; GetWindowRect(host, &bounds);
          const desktop_groups::GlassArea areas[]{{bounds, 8}};
          if (!glass.update(desktop, desktop_groups::wallpaperBackdrop(), areas) || !glass.handle()) result = 12;
          if (glass.handle()) {
            const HWND layers[]{window, glass.handle()};
            desktop_groups::lowerDesktopWindows(desktop, layers);
          }
        }
        desktop_groups::focusDesktopWindow(window, host);
        surface.focus(); pump(100);
        PostMessageW(GetFocus(), WM_CHAR, L's', 1); pump(100);
        PostMessageW(GetFocus(), WM_CHAR, L'j', 1); pump(100);
        if (found != -1 || characters != L"sj" || query.query() != L"sj") {
          std::cout << "query=" << ascii(query.query())
                    << " focus=" << GetFocus() << " foreground=" << GetForegroundWindow()
                    << " root=" << GetAncestor(window, GA_ROOT) << '\n';
          log("FAIL: WinUI characters do not reach incremental find"); result = 8;
        }
        const auto press = [&](UINT key) {
          const auto scanCode = static_cast<LPARAM>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC)) << 16;
          PostMessageW(GetFocus(), WM_KEYDOWN, key, scanCode | 1); pump(50);
          PostMessageW(GetFocus(), WM_KEYUP, key, scanCode | 0xc0000001); pump(50);
        };
        press(VK_RETURN);
        if (found != 0 || opened) { log("FAIL: candidate confirmation must locate without opening"); result = 8; }
        PostMessageW(GetFocus(), WM_CHAR, L'x', 1); pump(50);
        press(VK_ESCAPE);
        PostMessageW(GetFocus(), WM_CHAR, L'c', 1); pump(100);
        PostMessageW(GetFocus(), WM_CHAR, L'q', 1); pump(100);
        if (found != 0 || query.query() != L"cq") {
          std::cout << "found=" << found << " query=" << ascii(query.query())
                    << " characters=" << ascii(characters) << '\n';
          log("FAIL: WinUI find reset and polyphony"); result = 9;
        }
        press(VK_RETURN);
        if (found != 1 || opened) { log("FAIL: second candidate confirmation"); result = 9; }
        if (desktopHost) {
          if (GetForegroundWindow() != GetAncestor(window, GA_ROOT)) {
            // Foreground lock/user activity is an unmet interactive precondition,
            // not a title-editor failure. Still verify OLE and destruction below.
            log("SKIP: desktop title editing needs foreground desktop; another app owns focus");
            if (!result) result = 77;
          } else {
            surface.select({1}); surface.focus(1); pump(50);
            if (surface.previewRow() != 1) { log("FAIL: desktop preview does not resolve the selected WinUI file"); result = 13; }
            surface.select({0}); surface.focus(0);
            if (surface.previewRow() != 0) { log("FAIL: desktop preview retains a stale selection"); result = 13; }
            surface.select({});
            if (surface.previewRow() != -1) { log("FAIL: empty selection must not resolve a preview file"); result = 13; }
            bool committed = false;
            if (!surface.beginTitleRename([&](std::wstring value) { committed = !value.empty(); })) {
              log("FAIL: desktop-hosted title editor cannot acquire focus"); result = 11;
            }
            pump(50);
            if (surface.previewRow()) { log("FAIL: title input must not become a file-preview shortcut"); result = 13; }
            press(VK_ESCAPE);
            if (committed) { log("FAIL: canceled title edit committed"); result = 11; }
          }
        }
        if (!result) log("PASS: actual WinUI character routing, selection, scrolling and query reset");
      }
      if (verify) {
        const auto island = FindWindowExW(host, nullptr, L"Microsoft.UI.Content.DesktopChildSiteBridge", nullptr);
        const auto registration = island ? RegisterDragDrop(island, target.Get()) : E_FAIL;
        if (registration == S_OK) RevokeDragDrop(island);
        if (registration != DRAGDROP_E_ALREADYREGISTERED) {
          log("FAIL: WinUI content HWND has no drop target"); result = 4;
        } else {
          // OLE exposes this in-process registration property. Verify both HWNDs
          // route to the same handler, including the real Shell data payload.
          const auto bound = reinterpret_cast<IDropTarget *>(GetPropW(island, L"OleDropTargetInterface"));
          if (bound != target.Get() || GetPropW(host, L"OleDropTargetInterface") != bound) {
            log("FAIL: host and content do not share the drop target"); result = 5;
          } else {
            const auto data = testData();
            DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
            bound->DragEnter(data.Get(), MK_LBUTTON, {}, &effect);
            bound->DragOver(MK_LBUTTON, {}, &effect);
            bound->Drop(data.Get(), 0, {}, &effect);
            if (effect != DROPEFFECT_COPY || drops != 1) result = 6;
          }
        }
      } else if (!findTest) {
        ShowWindow(window, SW_SHOWNORMAL);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          if (runtime.preTranslate(message)) continue;
          TranslateMessage(&message); DispatchMessageW(&message);
        }
      }
      // Explorer can destroy the desktop before the Surface owner notices.
      if (desktopHost) { RevokeDragDrop(host); DestroyWindow(window); }
    }
    RevokeDragDrop(host);
    DestroyWindow(window);
  }
  if (liveTargets != 0) { log("FAIL: drop registration leaked a target"); result = 7; }
  std::error_code error; std::filesystem::remove_all(root, error);
  OleUninitialize();
  if (!result && verify) log("PASS: native/WinUI routing, file/folder payload and target lifetime");
  return result;
}
