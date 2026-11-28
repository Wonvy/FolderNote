#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
HWND receiver = nullptr;
const HWND testViewer = reinterpret_cast<HWND>(static_cast<UINT_PTR>(1234));
const HWND otherWindow = reinterpret_cast<HWND>(static_cast<UINT_PTR>(5678));
bool running = true, deliver = true, visible = false, allowed = false;
ULONGLONG tick = 1000;
int requests = 0, activations = 0;
std::wstring received;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
HWND findSeer(LPCWSTR name, LPCWSTR title) {
  check(name && !wcscmp(name, L"SeerWindowClass") && !title, "use Seer's documented IPC endpoint");
  return running ? receiver : nullptr;
}
ULONGLONG clockTick() { return tick; }
DWORD seerProcess(HWND window, LPDWORD pid) {
  *pid = window == receiver || window == testViewer ? 123 : 456; return 1;
}
BOOL allowForeground(DWORD pid) { check(pid == 123, "grant activation only to Seer"); allowed = true; return TRUE; }
BOOL previewVisible(HWND window) { return window == otherWindow || (window == testViewer && visible); }
[[maybe_unused]] LONG_PTR windowStyle(HWND window, int) { return window == testViewer ? WS_EX_NOACTIVATE : 0; }
BOOL enumerateWindows(WNDENUMPROC callback, LPARAM context) {
  for (const auto window : {receiver, otherWindow, testViewer}) if (!callback(window, context)) return FALSE;
  return TRUE;
}
BOOL activatePreview(HWND window) {
  check(window == testViewer && visible && allowed, "activate the visible testViewer, not the hidden IPC endpoint");
  ++activations; return TRUE;
}
LRESULT boundedSend(HWND window, UINT message, WPARAM w, LPARAM l, UINT flags, UINT timeout, PDWORD_PTR reply) {
  check(message == WM_COPYDATA && !w && (flags & SMTO_ABORTIFHUNG) && (flags & SMTO_BLOCK) && timeout <= 300,
        "preview IPC is bounded and does not pump unrelated box messages");
  return deliver ? SendMessageTimeoutW(window, message, w, l, flags, timeout, reply) : 0;
}
LRESULT CALLBACK receive(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message != WM_COPYDATA) return DefWindowProcW(window, message, w, l);
  const auto &data = *reinterpret_cast<COPYDATASTRUCT *>(l);
  check(data.dwData == 5000 && data.lpData && data.cbData >= sizeof(wchar_t) && data.cbData % sizeof(wchar_t) == 0,
        "send the official preview command with a UTF-16 payload");
  const auto text = static_cast<const wchar_t *>(data.lpData);
  const auto length = data.cbData / sizeof(wchar_t);
  check(text[length - 1] == 0, "include the path terminator in the IPC size");
  check(allowed, "grant Seer foreground permission before invoking it");
  const std::wstring next(text, length - 1);
  visible = !visible || received != next;
  received = next; ++requests;
  return 0; // Real Seer 4.8 acknowledges delivery with a zero window result.
}
}
#define FindWindowW findSeer
#define GetTickCount64 clockTick
#define SendMessageTimeoutW boundedSend
#define GetWindowThreadProcessId seerProcess
#define AllowSetForegroundWindow allowForeground
#define IsWindowVisible previewVisible
#define SetForegroundWindow activatePreview
#define EnumWindows enumerateWindows
#define GetWindowLongPtrW windowStyle
#include "../src/desktop_groups_preview.cpp"
#undef GetWindowLongPtrW
#undef EnumWindows
#undef SetForegroundWindow
#undef IsWindowVisible
#undef AllowSetForegroundWindow
#undef GetWindowThreadProcessId
#undef SendMessageTimeoutW
#undef GetTickCount64
#undef FindWindowW

int main() {
  const auto root = std::filesystem::temp_directory_path() /
      (L"ShibaKitPreviewTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  int result = 0;
  try {
    check(std::filesystem::create_directory(root), "create isolated preview files");
    const auto file = root / L"空格 preview.txt";
    std::ofstream(file) << "synthetic preview content";
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = receive;
    cls.lpszClassName = L"ShibaKit.PreviewTests";
    check(RegisterClassW(&cls) != 0, "register the isolated receiver");
    receiver = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    check(receiver != nullptr, "create the isolated receiver without contacting the user's Seer");
    const auto clipboard = GetClipboardSequenceNumber();
    check(desktop_groups::desktopPreviewAvailable(), "consume Space only while a previewer is available");
    check(!desktop_groups::desktopPreviewWindow(), "hidden Seer and unrelated visible apps do not change box stacking");
    check(desktop_groups::previewDesktopItem(file.native()) && requests == 1 && received == file.native() && activations == 1,
          "zero-return Seer receives the full Unicode and space-containing path");
    check(desktop_groups::desktopPreviewWindow() == testViewer, "recognize a non-activating preview independently of its trigger or foreground");
    tick += 199;
    check(desktop_groups::previewDesktopItem(file.native()) && requests == 1,
          "rapid repeats are consumed without toggling Seer repeatedly");
    ++tick;
    check(desktop_groups::previewDesktopItem(file.native()) && requests == 2 && activations == 1 && !visible,
          "a second press after the minimum interval reaches Seer's toggle command");
    tick += 200;
    check(desktop_groups::previewDesktopItem(root.native()) && requests == 3 && received == root.native(),
          "folders can also be previewed");
    tick += 200; running = false;
    check(!desktop_groups::desktopPreviewAvailable(), "without Seer keep normal selection keys");
    check(!desktop_groups::previewDesktopItem(file.native()) && requests == 3,
          "without Seer preserve the list's normal key behavior");
    running = true; deliver = false;
    check(!desktop_groups::previewDesktopItem(file.native()) && requests == 3,
          "an unresponsive or blocked previewer does not report success");
    deliver = true;
    check(desktop_groups::previewDesktopItem(file.native()) && requests == 4,
          "failed delivery does not throttle the next valid request");
    tick += 200;
    check(desktop_groups::previewDesktopItem(root.native()) && requests == 5 && received == root.native() && visible,
          "changing selection replaces the preview path while the testViewer is already open");
    tick += 200;
    auto invalid = file.native(); invalid.push_back(0); invalid += L"suffix";
    check(!desktop_groups::previewDesktopItem({}) && !desktop_groups::previewDesktopItem(invalid) &&
          !desktop_groups::previewDesktopItem(std::wstring(32768, L'a')) &&
          !desktop_groups::previewDesktopItem((root / L"missing.txt").native()) &&
          !desktop_groups::previewDesktopItem(L"::{645FF040-5081-101B-9F08-00AA002F954E}") && requests == 5,
          "invalid paths and virtual desktop items never reach the previewer");
    check(GetClipboardSequenceNumber() == clipboard, "preview integration leaves the clipboard untouched");
    std::cout << "PASS Seer IPC: Unicode paths, toggling, repeat suppression, failures and clipboard preservation\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
  if (receiver) DestroyWindow(receiver);
  UnregisterClassW(L"ShibaKit.PreviewTests", GetModuleHandleW(nullptr));
  std::error_code error; std::filesystem::remove_all(root, error);
  return result;
}
