#include "../src/desktop_groups_preview_keys.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
HOOKPROC installed = nullptr;
int chained = 0, posts = 0, installs = 0, unhooks = 0;
bool seerAvailable = true, modifiers = false, canPost = true;
const HWND receiver = reinterpret_cast<HWND>(UINT_PTR(1));
constexpr UINT dispatchMessage = WM_APP + 8;
HHOOK install(int type, HOOKPROC callback, HINSTANCE, DWORD thread) {
  check(type == WH_KEYBOARD_LL && thread == 0, "intercept before the external desktop preview hook");
  installed = callback;
  return reinterpret_cast<HHOOK>(UINT_PTR(++installs));
}
BOOL unhook(HHOOK) { ++unhooks; return TRUE; }
LRESULT next(HHOOK, int, WPARAM, LPARAM) { ++chained; return 0; }
SHORT keyState(int) { return modifiers ? SHORT(-32768) : 0; }
BOOL post(HWND window, UINT message, WPARAM, LPARAM) {
  check(window == receiver && message == dispatchMessage, "post only to the owning UI thread");
  if (canPost) ++posts;
  return canPost;
}
LRESULT press(WPARAM event, DWORD key = VK_SPACE, DWORD flags = 0, int code = HC_ACTION) {
  KBDLLHOOKSTRUCT input{}; input.vkCode = key; input.flags = flags;
  return installed(code, event, reinterpret_cast<LPARAM>(&input));
}
}
namespace desktop_groups { bool desktopPreviewAvailable() { return seerAvailable; } }
#define SetWindowsHookExW install
#define UnhookWindowsHookEx unhook
#define CallNextHookEx next
#define GetAsyncKeyState keyState
#define PostMessageW post
#include "../src/desktop_groups_preview_keys.cpp"
#undef SetWindowsHookExW
#undef UnhookWindowsHookEx
#undef CallNextHookEx
#undef GetAsyncKeyState
#undef PostMessageW

int main() {
  using namespace desktop_groups;
  try {
    const PreviewTarget first{receiver, reinterpret_cast<HWND>(UINT_PTR(2)), L"C:\\Box A\\选中图片.png"};
    auto second = first; second.path = L"C:\\Box B\\另一个文件.pdf";
    std::optional<PreviewTarget> target = first;
    {
      PreviewKeys keys(receiver, dispatchMessage, [&] { return target; });
      check(installed && installs == 1, "install scoped keyboard routing");
      check(press(WM_KEYDOWN) == 1 && posts == 0 && chained == 0 && !keys.take(), "box press never reaches Seer's native desktop handler");
      check(press(WM_KEYDOWN) == 1 && posts == 0, "held Space never repeats the preview");
      check(press(WM_KEYUP) == 1 && posts == 1 && chained == 0, "consume release and defer IPC until the hook has returned");
      check(keys.take() == first.path && !keys.take(), "dispatch the selected box path exactly once");
      target = second;
      check(press(WM_KEYDOWN) == 1 && press(WM_KEYUP) == 1 && keys.take() == second.path, "a new selection supplies the new path");
      target.reset();
      check(press(WM_KEYDOWN) == 0 && press(WM_KEYUP) == 0 && chained == 2 && !keys.take(), "native desktop and text editors retain their original key routing");
      target = first; modifiers = true;
      check(press(WM_KEYDOWN) == 0 && press(WM_KEYUP) == 0 && !keys.take(), "Ctrl Shift Alt Win combinations are not preview shortcuts");
      modifiers = false;
      check(press(WM_SYSKEYDOWN, VK_SPACE, LLKHF_ALTDOWN) == 0 && press(WM_SYSKEYUP, VK_SPACE, LLKHF_ALTDOWN) == 0, "preserve Alt Space even before async state updates");
      check(press(WM_KEYDOWN) == 1, "start scoped gesture");
      target.reset();
      check(press(WM_KEYUP) == 1 && !keys.take(), "focus changes cancel preview and consume the orphan release");
      target = first;
      check(press(WM_KEYDOWN) == 1, "start selected path snapshot");
      target = second;
      check(press(WM_KEYUP) == 1 && !keys.take(), "selection changes mid-gesture never preview a stale path");
      check(press(WM_KEYDOWN) == 1 && press(WM_KEYUP) == 1, "queue selected preview");
      target = first;
      check(!keys.take(), "selection changes before dispatch also invalidate queued preview");
      check(press(WM_KEYDOWN) == 1 && press(WM_KEYDOWN, VK_LCONTROL) == 0 && press(WM_KEYUP, VK_LCONTROL) == 0 &&
            press(WM_KEYUP) == 1 && !keys.take(), "a modifier pressed during the gesture cancels preview");
      target->path.clear();
      check(press(WM_KEYDOWN) == 1 && press(WM_KEYUP) == 1 && !keys.take(), "an empty box selection never falls back to the desktop file");
      target = first; seerAvailable = false;
      check(press(WM_KEYDOWN) == 0 && press(WM_KEYUP) == 0 && !keys.take(), "without Seer preserve normal list selection");
      seerAvailable = true; canPost = false;
      check(press(WM_KEYDOWN) == 1 && press(WM_KEYUP) == 1 && !keys.take(), "failed posting cannot leave a stale preview");
      canPost = true;
      check(keys.refresh() && installs == 2 && unhooks == 1, "refresh priority when Seer restarts or a box gains focus");
      check(press(WM_KEYDOWN, 'A') == 0 && press(WM_KEYDOWN, VK_SPACE, 0, -1) == 0, "pass unrelated keys and negative hook codes");
    }
    check(unhooks == installs, "remove every installed hook on shutdown");
    std::cout << "PASS scoped Space routing, selected paths, modifiers, cancellation and desktop isolation\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
