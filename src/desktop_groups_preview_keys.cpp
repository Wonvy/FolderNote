#include "desktop_groups_preview_keys.h"
#include "desktop_groups_preview.h"
#include <utility>

namespace desktop_groups {
namespace {
bool modifier(DWORD key) {
  return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
      key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
      key == VK_MENU || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN || key == VK_RWIN;
}
bool modified() {
  // Only inspect other keys; the current Space event's async state is not
  // updated until after LowLevelKeyboardProc returns.
  return ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_MENU) |
           GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
}
}
thread_local PreviewKeys *PreviewKeys::active = nullptr;
PreviewKeys::PreviewKeys(HWND receiver, UINT message, Resolve resolve)
    : receiver(receiver), message(message), resolve(std::move(resolve)) { refresh(); }
PreviewKeys::~PreviewKeys() {
  if (hook) UnhookWindowsHookEx(hook);
  if (active == this) active = nullptr;
}
bool PreviewKeys::refresh() {
  // Refresh on box focus and Seer's ready broadcast, also covering a Seer
  // restart that installed its desktop hook after FolderNote started.
  if (active && active != this) return false;
  const auto next = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard, GetModuleHandleW(nullptr), 0);
  if (!next) return false;
  if (hook) UnhookWindowsHookEx(hook);
  hook = next; active = this;
  return true;
}
bool PreviewKeys::key(WPARAM event, const KBDLLHOOKSTRUCT &input) {
  const bool down = event == WM_KEYDOWN || event == WM_SYSKEYDOWN;
  const bool up = event == WM_KEYUP || event == WM_SYSKEYUP;
  if (!down && !up) return false;
  if (input.vkCode != VK_SPACE) {
    if (captured && down && (modifier(input.vkCode) || input.vkCode == VK_ESCAPE)) canceled = true;
    return false;
  }
  if (down) {
    if (held) return captured;
    held = true; captured = canceled = false; pressed.reset();
    if (modified() || (input.flags & LLKHF_ALTDOWN)) return false;
    const auto target = resolve();
    if (!target || !desktopPreviewAvailable()) return false;
    pressed = target; captured = true;
    return true;
  }
  held = false;
  if (!std::exchange(captured, false)) return false;
  auto target = std::exchange(pressed, std::nullopt);
  // Consume the paired release even after focus/selection changes, but never
  // preview a stale item or let an orphan key-up preview a desktop selection.
  if (!canceled && !modified() && !(input.flags & LLKHF_ALTDOWN) && target &&
      !target->path.empty() && target == resolve()) {
    pending = std::move(target);
    if (!PostMessageW(receiver, message, 0, 0)) pending.reset();
  }
  return true;
}
std::optional<std::wstring> PreviewKeys::take() {
  const auto target = std::exchange(pending, std::nullopt);
  try {
    if (target && target == resolve()) return target->path;
  } catch (...) {}
  return std::nullopt;
}
LRESULT CALLBACK PreviewKeys::keyboard(int code, WPARAM event, LPARAM data) {
  if (code == HC_ACTION && active) {
    const bool wasCaptured = active->captured;
    try { if (active->key(event, *reinterpret_cast<const KBDLLHOOKSTRUCT *>(data))) return 1; }
    catch (...) {
      active->held = active->captured = active->canceled = false;
      active->pressed.reset();
      if (wasCaptured && reinterpret_cast<const KBDLLHOOKSTRUCT *>(data)->vkCode == VK_SPACE) return 1;
    }
  }
  return CallNextHookEx(nullptr, code, event, data);
}
}
