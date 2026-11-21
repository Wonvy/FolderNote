// Interactive OLE regression fixture. Drag the generated file/folder onto the
// native rim or the real WinUI box content. Does not move any user files.
#include "../src/desktop_groups_winui.h"
#include "../src/desktop_groups_preview.h"
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
  const bool verify = mode == "--verify" || mode == "--verify-parent-only";
  const bool parentOnly = mode == "--parent-only" || mode == "--verify-parent-only";
  const bool preview = mode == "--preview" || mode == "--preview-list";
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
    HWND window = CreateWindowExW(preview ? WS_EX_NOACTIVATE : 0, cls.lpszClassName, L"FolderNote Drop Probe",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 100, 100, 900, 600, nullptr, nullptr, cls.hInstance, nullptr);
    HWND host = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        300, 30, 550, 470, window, nullptr, cls.hInstance, nullptr);
    statusWindow = CreateWindowExW(0, L"STATIC", L"Successful drops: 0", WS_CHILD | WS_VISIBLE,
        25, 300, 260, 60, window, nullptr, cls.hInstance, nullptr);
    auto target = Microsoft::WRL::Make<Target>();
    std::cout << "Register native hr=" << std::hex << RegisterDragDrop(host, target.Get()) << std::dec << std::endl;
    {
      desktop_groups::winui::Events events;
      if (preview) events.previewAvailable = [] { return desktop_groups::desktopPreviewAvailable(); };
      if (preview) events.preview = [&](int row) {
        const bool handled = row >= 0 && row <= 1 && desktop_groups::previewDesktopItem(
            (root / (row == 0 ? L"sample.txt" : L"sample folder")).native());
        std::ofstream(root / L"trace.txt", std::ios::app) << "preview row=" << row << '\n';
        log(handled ? "Preview delivered" : "Preview unavailable");
        return handled;
      };
      desktop_groups::winui::Surface surface(host, std::move(events), parentOnly ? nullptr : target.Get());
      if (!surface.available()) { log("Surface unavailable"); return 3; }
      surface.place({10, 10, 540, 460});
      surface.appearance(false, false, RGB(235, 240, 245), 255, false);
      surface.header({L"WinUI drop target"}, 0, false, false, false, 8,
          RGB(0,0,0), RGB(80,80,80), RGB(120,120,120), RGB(0,120,200), 36,
          desktop_groups::TitleStyle::Text, nullptr);
      std::vector<desktop_groups::winui::Item> items{{preview ? L"sample.txt" : L"Existing item", (root / L"sample.txt").native(), nullptr}};
      if (preview) items.push_back({L"sample folder", (root / L"sample folder").native(), nullptr});
      surface.rows(items,
                   mode == "--preview-list", 32);
      if (preview) surface.select({0});
      surface.show(true);
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
      } else {
        ShowWindow(window, SW_SHOWNORMAL);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          if (runtime.preTranslate(message)) continue;
          TranslateMessage(&message); DispatchMessageW(&message);
        }
      }
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
