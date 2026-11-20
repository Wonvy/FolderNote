#include "../src/quick_dialog.h"
#include "../src/quick_folders.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
int checks = 0;
void check(bool value, const char *reason) { ++checks; if (!value) throw std::runtime_error(reason); }
HWND targetWindow() { return reinterpret_cast<HWND>(0x101); }
HWND addressWindow() { return reinterpret_cast<HWND>(0x102); }
HWND filenameWindow() { return reinterpret_cast<HWND>(0x103); }
HWND otherWindow() { return reinterpret_cast<HWND>(0x104); }
HWND addressCombo() { return reinterpret_cast<HWND>(0x105); }
struct Message { HWND window; UINT kind; WPARAM key; std::wstring text; };
struct Simulation {
  HWND foreground = otherWindow(), focus = filenameWindow(), foregroundAfterActivate = nullptr;
  DWORD process = 71, thread = 81;
  bool dialogValid = true, addressValid = true, activationAccepted = true, acknowledgement = true;
  bool guiInfo = true, writeDelivered = true, writeAccepted = true, enterDelivered = true, keyUpDelivered = true;
  bool enumerateAddress = true, focusDelivered = true, focusChanges = true;
  int modifier = 0, activations = 0, guiQueries = 0, globalInputs = 0;
  bool addressVisible = true, revealOnF4 = false, postAccepted = true;
  int addressShortcuts = 0;
  DWORD slept = 0;
  std::vector<HWND> enumerated;
  std::vector<Message> messages;
  std::vector<std::string> actions;
  std::function<void()> afterAcknowledgement, afterFocusRequest, afterWrite, beforeFocus;
} simulation;
DWORD fakeWindowProcess(HWND window, DWORD *process) {
  if (window != targetWindow()) { if (process) *process = 0; return 0; }
  if (process) *process = simulation.process;
  return simulation.thread;
}
BOOL fakeForeground(HWND window) {
  check(window == targetWindow(), "activation targets only the original dialog");
  ++simulation.activations; simulation.actions.push_back("activate");
  simulation.foregroundAfterActivate = simulation.foreground;
  return simulation.activationAccepted;
}
HWND fakeGetForeground() { return simulation.foreground; }
SHORT fakeKeys(int key) { return key == simulation.modifier ? SHORT(-32768) : 0; }
[[maybe_unused]] UINT fakeInput(UINT, LPINPUT, int) {
  ++simulation.globalInputs;
  return 0;
}
BOOL fakeChildren(HWND window, WNDENUMPROC callback, LPARAM data) {
  check(window == targetWindow(), "address discovery stays inside the original dialog");
  simulation.enumerated.push_back(filenameWindow());
  if (!callback(filenameWindow(), data)) return FALSE;
  if (simulation.enumerateAddress) {
    simulation.enumerated.push_back(addressWindow());
    if (!callback(addressWindow(), data)) return FALSE;
  }
  return TRUE;
}
BOOL fakeVisible(HWND window) { return window == addressWindow() && simulation.addressVisible; }
HWND fakeParent(HWND window) { return window == addressWindow() ? addressCombo() : targetWindow(); }
int fakeClassName(HWND window, LPWSTR buffer, int capacity) {
  if (window != addressCombo() || capacity < 9) return 0;
  return static_cast<int>(wcscpy_s(buffer, capacity, L"ComboBox") == 0 ? 8 : 0);
}
BOOL fakePost(HWND window, UINT message, WPARAM key, LPARAM) {
  check(window == targetWindow() && key == VK_F4 && (message == WM_KEYDOWN || message == WM_KEYUP),
        "address accelerator is queued only on the original dialog");
  if (!simulation.postAccepted) return FALSE;
  if (message == WM_KEYDOWN) {
    ++simulation.addressShortcuts;
    if (simulation.revealOnF4) { simulation.addressVisible = true; simulation.focus = addressWindow(); }
  }
  return TRUE;
}
BOOL fakeThreadInfo(DWORD thread, LPGUITHREADINFO info) {
  check(thread == 81 && info && info->cbSize == sizeof(GUITHREADINFO), "focus query uses the captured dialog thread");
  ++simulation.guiQueries;
  if (simulation.beforeFocus) simulation.beforeFocus();
  if (!simulation.guiInfo) return FALSE;
  info->hwndFocus = simulation.focus; return TRUE;
}
void fakeSleep(DWORD milliseconds) {
  check(milliseconds <= 10, "address focus polling has short bounded waits");
  simulation.slept += milliseconds;
}
LRESULT fakeMessage(HWND window, UINT kind, WPARAM key, LPARAM data, UINT flags, UINT timeout, PDWORD_PTR result) {
  check((flags & SMTO_ABORTIFHUNG) && timeout && timeout <= 1000, "dialog messages have bounded waits");
  Message message{window, kind, key, {}};
  if (kind == WM_SETTEXT) message.text = reinterpret_cast<const wchar_t *>(data);
  simulation.messages.push_back(std::move(message));
  if (result) *result = 0;
  if (kind == WM_NULL) {
    check(window == targetWindow(), "activation acknowledgement goes only to the target dialog");
    simulation.actions.push_back("acknowledge");
    if (!simulation.acknowledgement) return 0;
    if (simulation.activationAccepted) simulation.foreground = targetWindow();
    if (simulation.afterAcknowledgement) simulation.afterAcknowledgement();
    return 1;
  }
  if (kind == WM_NEXTDLGCTL) {
    check(window == targetWindow() && reinterpret_cast<HWND>(key) == addressWindow() && data == TRUE,
          "dialog-local focus request names only the verified address edit");
    simulation.actions.push_back("focus-address");
    if (!simulation.focusDelivered) return 0;
    if (simulation.focusChanges) simulation.focus = addressWindow();
    if (simulation.afterFocusRequest) simulation.afterFocusRequest();
    return 1;
  }
  check(window == addressWindow(), "text and Enter must never target the filename field or dialog");
  if (kind == WM_SETTEXT) {
    simulation.actions.push_back("text");
    if (result) *result = simulation.writeAccepted;
    if (simulation.afterWrite) simulation.afterWrite();
    return simulation.writeDelivered;
  }
  check((kind == WM_KEYDOWN || kind == WM_KEYUP) && key == VK_RETURN, "only Enter is submitted directly to the address edit");
  simulation.actions.push_back(kind == WM_KEYDOWN ? "enter-down" : "enter-up");
  return kind == WM_KEYDOWN ? simulation.enterDelivered : simulation.keyUpDelivered;
}
}

namespace quick {
bool isFileDialog(HWND window) { return window == targetWindow() && simulation.dialogValid; }
bool addressEdit(HWND edit, HWND dialog) {
  return edit == addressWindow() && dialog == targetWindow() && simulation.addressValid;
}
}
// All target HWNDs are inert sentinels. Production navigation uses these
// wrappers; no real foreground window, keyboard input, dialog or setting changes.
#define GetWindowThreadProcessId fakeWindowProcess
#define SetForegroundWindow fakeForeground
#define GetForegroundWindow fakeGetForeground
#define IsWindowVisible fakeVisible
#define GetParent fakeParent
#define GetClassNameW fakeClassName
#define PostMessageW fakePost
#define GetAsyncKeyState fakeKeys
#define EnumChildWindows fakeChildren
#define SendInput fakeInput
#define GetGUIThreadInfo fakeThreadInfo
#define Sleep fakeSleep
#define SendMessageTimeoutW fakeMessage
#include "../src/quick_dialog.cpp"
#undef SendMessageTimeoutW
#undef Sleep
#undef GetGUIThreadInfo
#undef SendInput
#undef EnumChildWindows
#undef IsWindowVisible
#undef GetParent
#undef GetClassNameW
#undef PostMessageW
#undef GetAsyncKeyState
#undef GetForegroundWindow
#undef SetForegroundWindow
#undef GetWindowThreadProcessId

namespace {
using Result = quick::dialog::NavigateResult;
struct Request {
  std::wstring name;
  quick::detail::MenuRequest current, replacement;
  static std::wstring nextName() {
    static unsigned serial = 0;
    return L"Local\\FolderNote.QuickDialogTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++serial);
  }
  Request() : name(nextName()), current(name.c_str()), replacement(name.c_str()) {
    simulation = {};
    check(current.valid() && replacement.valid(), "create isolated request revisions"); current.begin();
  }
  Result run() {
    auto result = quick::dialog::navigate(targetWindow(), L"D:\\测试 folder\\目标", current);
    check(!simulation.globalInputs, "navigation never uses global SendInput for address activation or Enter");
    return result;
  }
  void cancel() { replacement.begin(); }
};
size_t count(UINT kind) {
  return std::count_if(simulation.messages.begin(), simulation.messages.end(), [=](const Message &message) { return message.kind == kind; });
}
void noEdits(const char *reason) { check(!count(WM_SETTEXT) && !count(WM_KEYDOWN) && !count(WM_KEYUP), reason); }
void noInput(const char *reason) { check(!simulation.globalInputs, reason); noEdits(reason); }
void activation() {
  {
    Request request;
    check(request.run() == Result::Success, "asynchronous activation succeeds after the target processes WM_NULL");
    check(simulation.foregroundAfterActivate == otherWindow() && simulation.foreground == targetWindow(),
          "activation request alone does not prematurely report the dialog as foreground");
    check(simulation.actions == std::vector<std::string>{"activate", "acknowledge", "focus-address", "text", "enter-down", "enter-up"},
          "activation acknowledgement precedes scoped address focus, write and Enter");
    check(simulation.enumerated == std::vector<HWND>{filenameWindow(), addressWindow()} && count(WM_NEXTDLGCTL) == 1,
          "address discovery skips the filename edit even when it is enumerated first");
    auto written = std::find_if(simulation.messages.begin(), simulation.messages.end(), [](const Message &message) { return message.kind == WM_SETTEXT; });
    check(written != simulation.messages.end() && written->text == L"D:\\测试 folder\\目标" && count(WM_KEYDOWN) == 1 && count(WM_KEYUP) == 1,
          "valid address receives the exact Unicode path and one Enter pair");
  }
  {
    Request request; simulation.activationAccepted = false;
    check(request.run() == Result::ActivationFailed, "denied foreground activation fails");
    noInput("denied activation never types or submits an address");
  }
  {
    Request request; simulation.acknowledgement = false;
    check(request.run() == Result::ActivationFailed, "hung target fails its bounded activation acknowledgement");
    noInput("hung target never receives keyboard input or address changes");
  }
  for (int modifier : {VK_CONTROL, VK_SHIFT, VK_MENU}) {
    Request request; simulation.modifier = modifier;
    check(request.run() == Result::ModifierPressed, "held modifier cancels navigation");
    noInput("held modifier prevents address manipulation");
  }
}
void cancellation() {
  {
    Request request; request.cancel();
    check(request.run() == Result::TargetChanged && !simulation.activations, "stale menu request cannot activate a target");
    noInput("stale request never changes the dialog");
  }
  {
    Request request; simulation.afterAcknowledgement = [&] { request.cancel(); };
    check(request.run() == Result::TargetChanged, "request replaced during activation cancels navigation");
    noInput("cancelled activation never manipulates the address control");
  }
  for (bool process : {false, true}) {
    Request request; simulation.afterAcknowledgement = [process] { if (process) ++simulation.process; else ++simulation.thread; };
    check(request.run() == Result::TargetChanged, "target process or thread replacement cancels activation");
    noInput("reused target identity never receives keyboard input");
  }
  {
    Request request; simulation.afterFocusRequest = [&] { request.cancel(); };
    check(request.run() == Result::TargetChanged, "new request during scoped focus cancels the old address command");
    noEdits("cancelled address command never writes text or presses Enter");
  }
}
void focusSafety() {
  {
    Request request;
    simulation.afterWrite = [] { simulation.focus = addressCombo(); };
    simulation.beforeFocus = [] {
      if (simulation.slept) simulation.focus = addressWindow();
    };
    check(request.run() == Result::Success && simulation.slept == 10 && count(WM_KEYDOWN) == 1,
          "cross-process address update can temporarily focus the parent combo before Enter");
  }
  {
    Request request;
    simulation.afterWrite = [] { simulation.focus = addressCombo(); };
    check(request.run() == Result::TargetChanged && simulation.slept <= 500 && !count(WM_KEYDOWN),
          "a combo that keeps focus never receives Enter");
  }
  {
    Request request;
    simulation.afterWrite = [] { simulation.focus = addressCombo(); };
    simulation.beforeFocus = [] {
      if (simulation.slept) simulation.focus = filenameWindow();
    };
    check(request.run() == Result::TargetChanged && simulation.slept == 10 && !count(WM_KEYDOWN),
          "focus leaving the address combo for the filename cancels submission immediately");
  }
  for (int mode = 0; mode < 3; ++mode) {
    Request request;
    simulation.enumerateAddress = mode != 0;
    simulation.addressVisible = mode != 1;
    simulation.focusChanges = false;
    simulation.revealOnF4 = true;
    check(request.run() == Result::Success && simulation.addressShortcuts == 1 && count(WM_SETTEXT) == 1,
          "missing, hidden or initially unfocusable address edit becomes available after local F4");
  }
  {
    Request request; simulation.enumerateAddress = false; simulation.postAccepted = false;
    check(request.run() == Result::InputBlocked, "blocked address accelerator is reported");
    noEdits("blocked activation cannot submit the dialog");
  }
  {
    Request request; simulation.focusChanges = false;
    check(request.run() == Result::AddressUnavailable, "filename focus is never mistaken for the address edit");
    noEdits("filename focus receives neither path text nor Enter");
    check(simulation.slept <= 1000 && simulation.guiQueries <= 100, "missing address focus has a bounded polling interval");
  }
  {
    Request request; simulation.guiInfo = false;
    check(request.run() == Result::AddressUnavailable, "unreadable focus information leaves the dialog unchanged");
    noEdits("failed focus query never writes or submits text");
    check(simulation.slept <= 1000, "failed focus query cannot wait indefinitely");
  }
  for (bool replaceFocus : {false, true}) {
    Request request;
    simulation.beforeFocus = [replaceFocus] {
      if (simulation.guiQueries != 2) return;
      if (replaceFocus) simulation.focus = filenameWindow();
      else simulation.addressValid = false;
    };
    check(request.run() == Result::TargetChanged && simulation.guiQueries == 2,
          "address identity or focus changing immediately before writing cancels navigation");
    noEdits("pre-write revalidation prevents all text and Enter when the address control changes");
  }
  for (int change = 0; change < 6; ++change) {
    Request request;
    simulation.afterWrite = [&, change] {
      if (change == 0) simulation.focus = filenameWindow();
      if (change == 1) simulation.foreground = otherWindow();
      if (change == 2) ++simulation.process;
      if (change == 3) ++simulation.thread;
      if (change == 4) simulation.addressValid = false;
      if (change == 5) request.cancel();
    };
    check(request.run() == Result::TargetChanged, "focus, identity or request change during address write cancels submission");
    check(count(WM_SETTEXT) == 1 && !count(WM_KEYDOWN) && !count(WM_KEYUP), "changed target never receives Enter after address write");
  }
}
void failures() {
  {
    Request request; simulation.enumerateAddress = false;
    check(request.run() == Result::AddressUnavailable && !count(WM_NEXTDLGCTL), "missing address control never falls back to the filename edit");
    noEdits("missing address control leaves the dialog unchanged");
  }
  {
    Request request; simulation.focusDelivered = false;
    check(request.run() == Result::InputBlocked && count(WM_NEXTDLGCTL) == 1, "failed scoped focus request is reported");
    noEdits("failed focus request never writes or submits an address");
    check(simulation.guiQueries == 0 && simulation.slept == 0, "failed focus delivery does not attempt global input fallback");
  }
  for (bool delivered : {false, true}) {
    Request request; simulation.writeDelivered = delivered; simulation.writeAccepted = false;
    check(request.run() == Result::AddressWriteFailed, "address write timeout or rejection is reported");
    check(count(WM_SETTEXT) == 1 && !count(WM_KEYDOWN) && !count(WM_KEYUP), "failed address write cannot trigger submission");
  }
  {
    Request request; simulation.enterDelivered = false;
    check(request.run() == Result::AddressSubmitFailed && count(WM_KEYDOWN) == 1 && !count(WM_KEYUP),
          "failed Enter delivery is reported without a second input path");
  }
  {
    Request request; simulation.keyUpDelivered = false;
    check(request.run() == Result::Success && count(WM_KEYDOWN) == 1 && count(WM_KEYUP) == 1,
          "accepted Enter remains submitted if the address edit disappears before key release");
  }
}
}
int main() {
  try {
    activation(); cancellation(); focusSafety(); failures();
    std::cout << "PASS " << checks << " dialog navigation checks: activation, identity, cancellation, focus safety, input and submission failures\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; return 1; }
}
