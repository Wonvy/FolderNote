#include "../src/tabs_routing.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
int checks = 0;
void check(bool value, const char *reason) { ++checks; if (!value) throw std::runtime_error(reason); }
bool feature = true, validImage = true, sendSucceeds = true, capabilitySucceeds = true;
ULONGLONG elapsed = 0;
HWND foreground = nullptr, foreign = nullptr;
struct Hook { DWORD first, last; WINEVENTPROC callback; };
std::vector<Hook> hooks;
std::vector<HWND> windows, visible, capabilityQueries, released;
struct Route { HWND source, target, helper; };
std::vector<Route> routes;
DWORD_PTR routeReply = 1, capabilityReply = 2;
std::function<void()> duringSend, duringCapability;
BOOL fakeEnumWindows(WNDENUMPROC callback, LPARAM value) {
  auto snapshot = windows;
  for (HWND window : snapshot) if (IsWindow(window) && !callback(window, value)) return FALSE;
  return TRUE;
}
BOOL fakeVisible(HWND window) { return std::find(visible.begin(), visible.end(), window) != visible.end(); }
HWND fakeForeground() { return foreground; }
DWORD fakeWindowProcess(HWND window, DWORD *process) {
  DWORD thread = GetWindowThreadProcessId(window, process);
  if (window == foreign && process) *process = 0xfffffffe;
  return thread;
}
BOOL fakeImage(HANDLE, DWORD, LPWSTR text, PDWORD count) {
  wchar_t system[MAX_PATH]{}; GetWindowsDirectoryW(system, ARRAYSIZE(system));
  auto path = std::wstring(system) + (validImage ? L"\\explorer.exe" : L"\\another.exe");
  if (!count || *count <= path.size()) return FALSE;
  memcpy(text, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); *count = DWORD(path.size()); return TRUE;
}
HWINEVENTHOOK fakeHook(DWORD first, DWORD last, HMODULE, WINEVENTPROC callback, DWORD, DWORD, DWORD) {
  hooks.push_back({first, last, callback}); return reinterpret_cast<HWINEVENTHOOK>(hooks.size());
}
BOOL fakeUnhook(HWINEVENTHOOK) { return TRUE; }
UINT_PTR fakeSetTimer(HWND, UINT_PTR id, UINT, TIMERPROC) { return id; }
BOOL fakeKillTimer(HWND, UINT_PTR) { return TRUE; }
ULONGLONG fakeTick() { return 100000 + elapsed; }
BOOL fakePidlEqual(PCIDLIST_ABSOLUTE left, PCIDLIST_ABSOLUTE right) {
  const auto size = ILGetSize(left);
  return size == ILGetSize(right) && !memcmp(left, right, size);
}
LRESULT fakeSend(HWND target, UINT message, WPARAM w, LPARAM l, UINT flags, UINT timeout, PDWORD_PTR result) {
  if (message == tabs::routing::capabilityMessage) {
    check((flags & SMTO_ABORTIFHUNG) && timeout && timeout <= 100, "capability probe has a short bounded wait");
    capabilityQueries.push_back(target);
    auto reenter = std::exchange(duringCapability, {});
    if (reenter) reenter();
    if (result) *result = capabilityReply;
    return capabilitySucceeds ? 1 : 0;
  }
  if (message != tabs::routing::routeMessage) return 0;
  routes.push_back({target, reinterpret_cast<HWND>(w), reinterpret_cast<HWND>(l)});
  auto reenter = std::exchange(duringSend, {});
  if (reenter) reenter();
  if (result) *result = routeReply;
  return sendSucceeds ? 1 : 0;
}
BOOL fakePost(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == tabs::routing::releaseGateMessage()) { released.push_back(window); return TRUE; }
  return PostMessageW(window, message, w, l);
}
}
namespace tabs::routing { bool testEnabled() { return feature; } }
// The real watcher runs only against hidden windows owned by this test. No
// global observers, real Explorer routing, settings writes or close messages.
#define enabled testEnabled
#define EnumWindows fakeEnumWindows
#define IsWindowVisible fakeVisible
#define GetForegroundWindow fakeForeground
#define GetWindowThreadProcessId fakeWindowProcess
#define QueryFullProcessImageNameW fakeImage
#define SetWinEventHook fakeHook
#define UnhookWinEvent fakeUnhook
#define SetTimer fakeSetTimer
#define KillTimer fakeKillTimer
#define GetTickCount64 fakeTick
#define SendMessageTimeoutW fakeSend
#define PostMessageW fakePost
#define ILIsEqual fakePidlEqual
#include "../src/tabs_router.cpp"
#undef ILIsEqual
#undef PostMessageW
#undef SendMessageTimeoutW
#undef GetTickCount64
#undef KillTimer
#undef SetTimer
#undef UnhookWinEvent
#undef SetWinEventHook
#undef QueryFullProcessImageNameW
#undef GetWindowThreadProcessId
#undef GetForegroundWindow
#undef IsWindowVisible
#undef EnumWindows
#undef enabled

namespace {
struct Frame {
  HWND window = nullptr, band = nullptr;
  explicit Frame(bool attach = true, const wchar_t *name = L"CabinetWClass") {
    window = CreateWindowW(name, L"Router test", WS_OVERLAPPED, 0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create hidden Explorer fixture");
    windows.push_back(window); visible.push_back(window);
    if (attach) attachBand();
  }
  void attachBand() {
    band = CreateWindowW(L"ShibaKit.ExplorerTabs.Band.v1", L"", WS_CHILD, 0, 0, 200, 30, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(band && SetPropW(window, tabs::windowProperty, band), "publish test band"); visible.push_back(band);
  }
  void hide() { std::erase(visible, window); std::erase(visible, band); }
  void gate() { check(SetPropW(window, tabs::routing::gateProperty, reinterpret_cast<HANDLE>(1)) != 0, "mark held source fixture"); }
  ~Frame() {
    std::erase(windows, window); std::erase(visible, window); std::erase(visible, band);
    if (IsWindow(window)) DestroyWindow(window);
  }
};
struct Watcher {
  HWND helper = nullptr;
  Watcher() {
    helper = CreateWindowW(L"STATIC", L"Router test helper", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(helper != nullptr && tabs::routing::start(helper), "start isolated routing watcher");
  }
  ~Watcher() {
    tabs::routing::stop(); DestroyWindow(helper); hooks.clear(); routes.clear();
    capabilityQueries.clear(); released.clear(); duringSend = {}; duringCapability = {};
    elapsed = 0; foreground = foreign = nullptr;
    feature = validImage = sendSucceeds = capabilitySucceeds = true; routeReply = 1; capabilityReply = 2;
  }
  void drain() {
    MSG message{};
    while (PeekMessageW(&message, helper, 0, 0, PM_REMOVE)) {
      LRESULT result = 0; tabs::routing::message(helper, message.message, message.wParam, message.lParam, result);
    }
  }
  void event(DWORD event, HWND window, LONG object = OBJID_WINDOW, LONG child = CHILDID_SELF) {
    const auto callbacks = hooks;
    for (const auto &hook : callbacks) if (event >= hook.first && event <= hook.last)
      hook.callback(nullptr, event, window, object, child, GetCurrentThreadId(), DWORD(fakeTick()));
    drain();
  }
  void tick() {
    LRESULT result = 0; tabs::routing::message(helper, WM_TIMER, 40, 0, result); drain();
  }
  bool hold(HWND source) {
    LRESULT result = -1;
    check(tabs::routing::message(helper, tabs::routing::holdMessage, reinterpret_cast<WPARAM>(source), 0, result),
          "routing owns the pre-show hold handshake");
    return result != 0;
  }
  ULONG_PTR data(HWND sender, ULONG_PTR kind, const void *bytes, DWORD count) {
    COPYDATASTRUCT data{kind, count, const_cast<void *>(bytes)}; LRESULT result = -1;
    check(tabs::routing::message(helper, WM_COPYDATA, reinterpret_cast<WPARAM>(sender), reinterpret_cast<LPARAM>(&data), result),
          "routing owns its copy-data protocol");
    return static_cast<ULONG_PTR>(result);
  }
};
void holds() {
  {
    Frame existing; Watcher watcher; Frame fresh(false); fresh.hide();
    check(watcher.hold(fresh.window) && routes.empty(), "hidden new source is approved before its band or create event arrives");
    check(capabilityQueries.size() == 1 && capabilityQueries[0] == existing.band, "hold verifies the retained destination protocol");
    watcher.tick();
    check(routes.empty(), "held source still waits for its own tab band");
    fresh.attachBand(); fresh.hide(); watcher.tick();
    check(routes.empty(), "hidden source without a confirmed gate is not routed");
    fresh.gate(); watcher.tick();
    check(routes.size() == 1 && routes[0].source == fresh.band && routes[0].target == existing.band && released.empty(),
          "hidden gated source routes without becoming visible during accepted import");
    check(capabilityQueries.size() == 1, "confirmed target capability is reused during routing");
    elapsed += 20000; watcher.tick();
    check(routes.size() == 1 && released == std::vector<HWND>{fresh.window}, "accepted transfer releases a surviving source at its original hold deadline");
    elapsed += 6000; watcher.tick();
    check(routes.size() == 1 && released.size() == 1, "accepted transfer quarantine expiry neither reroutes nor releases the source twice");
  }
  {
    Frame existing, baseline; baseline.hide(); Watcher watcher;
    check(!watcher.hold(baseline.window), "hidden baseline window is never held for merging");
    Frame shown;
    check(!watcher.hold(shown.window), "already visible new source cannot enter pre-show hold");
    Frame fake(true, L"STATIC"); fake.hide();
    check(!watcher.hold(fake.window), "non-Explorer class cannot request a visibility hold");
    Frame invalid; invalid.hide(); validImage = false;
    check(!watcher.hold(invalid.window), "Explorer-looking window from another image cannot request a hold");
  }
  {
    Watcher watcher; Frame first(false); first.hide();
    check(!watcher.hold(first.window), "first Explorer window without a retained destination remains visible");
  }
  {
    Frame existing; existing.gate(); Watcher watcher; Frame fresh(false); fresh.hide();
    check(!watcher.hold(fresh.window) && capabilityQueries.empty(), "a gated destination never justifies hiding another window");
  }
  {
    Frame existing; Watcher watcher; Frame fresh(false); fresh.hide(); capabilityReply = 0;
    check(!watcher.hold(fresh.window), "old destination protocol declines the pre-show hold");
    capabilityReply = 2;
    check(!watcher.hold(fresh.window) && capabilityQueries.size() == 1, "explicit unsupported capability is cached for the same destination band");
  }
  {
    Frame existing; Watcher watcher; Frame fresh(false); fresh.hide(); capabilitySucceeds = false;
    check(!watcher.hold(fresh.window), "capability timeout leaves the source unheld");
    capabilitySucceeds = true; elapsed += 5000;
    check(watcher.hold(fresh.window) && capabilityQueries.size() >= 2, "timed-out capability can be retried while the candidate is pending");
  }
  {
    Frame existing; Watcher watcher; Frame fresh(false); fresh.hide(); std::unique_ptr<Frame> reentrant;
    duringCapability = [&] {
      reentrant = std::make_unique<Frame>(false); reentrant->hide();
      watcher.event(EVENT_OBJECT_CREATE, reentrant->window);
    };
    check(watcher.hold(fresh.window) && reentrant, "capability query tolerates a reentrant candidate insertion");
    fresh.gate(); fresh.attachBand(); fresh.hide(); watcher.tick();
    check(routes.size() == 1 && routes[0].source == fresh.band && routes[0].target == existing.band,
          "reentrant capability query preserves the approved source and target identities");
  }
  {
    Frame existing; Watcher watcher; Frame fresh(false); fresh.hide();
    duringCapability = [] { tabs::routing::stop(); };
    check(!watcher.hold(fresh.window) && routes.empty(), "watcher stop during capability query revokes the hold approval");
  }
  {
    Frame existing; Watcher watcher; Frame fresh; fresh.hide();
    check(watcher.hold(fresh.window), "approve source for retained-route release check");
    fresh.gate(); routeReply = 2; watcher.tick(); watcher.tick();
    check(routes.size() == 1 && released == std::vector<HWND>{fresh.window}, "deliberately retained source is released exactly once");
  }
  {
    Frame existing; Watcher watcher; Frame fresh; fresh.hide();
    check(watcher.hold(fresh.window), "approve source for busy timeout release check");
    fresh.gate(); routeReply = 0; watcher.tick();
    check(routes.size() == 1 && released.empty(), "busy source stays held while its bounded retry interval remains");
    elapsed += 21000; watcher.tick();
    check(routes.size() == 1 && released == std::vector<HWND>{fresh.window}, "busy candidate timeout releases the source without another route attempt");
  }
  {
    Frame existing; Watcher watcher; Frame fresh(false); fresh.hide();
    check(watcher.hold(fresh.window), "approve source for watcher stop release check");
    fresh.gate(); tabs::routing::stop();
    check(released == std::vector<HWND>{fresh.window}, "watcher stop releases held sources even before their band exists");
  }
  {
    Frame existing; Watcher watcher; Frame fresh; fresh.hide();
    check(watcher.hold(fresh.window), "approve source for ambiguous route timeout check");
    fresh.gate(); elapsed += 3000; sendSucceeds = false; watcher.tick();
    check(routes.size() == 1 && released.empty(), "ambiguous route timeout keeps its source held because the import may be pending");
    sendSucceeds = true; watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    elapsed += 16999; watcher.tick();
    check(routes.size() == 1 && released.empty(), "ambiguous route is never retried or released before the original hold deadline");
    elapsed += 1; watcher.tick();
    check(routes.size() == 1 && released == std::vector<HWND>{fresh.window}, "ambiguous route releases exactly at the original twenty-second hold deadline");
    RemovePropW(fresh.window, tabs::routing::gateProperty); visible.push_back(fresh.window); visible.push_back(fresh.band);
    foreground = fresh.window; watcher.event(EVENT_SYSTEM_FOREGROUND, fresh.window);
    Frame duringQuarantine; watcher.event(EVENT_OBJECT_SHOW, duringQuarantine.window); watcher.tick();
    check(routes.size() == 2 && routes.back().source == duringQuarantine.band && routes.back().target == existing.band,
          "released ambiguous source remains excluded as a destination for twenty-five seconds after dispatch");
    elapsed += 8000; watcher.tick();
    check(routes.size() == 2 && released.size() == 1, "ambiguous source quarantine ends without repeating its transfer or release");
    foreground = fresh.window; watcher.event(EVENT_SYSTEM_FOREGROUND, fresh.window);
    Frame afterQuarantine; watcher.event(EVENT_OBJECT_SHOW, afterQuarantine.window); watcher.tick();
    check(routes.size() == 3 && routes.back().source == afterQuarantine.band && routes.back().target == fresh.band,
          "surviving ambiguous source is reusable only after its dispatch quarantine ends");
  }
}
void lifecycle() {
  {
    Frame existing, hidden;
    std::erase(visible, hidden.window); std::erase(visible, hidden.band);
    foreground = existing.window; Watcher watcher;
    visible.push_back(hidden.window); visible.push_back(hidden.band);
    watcher.event(EVENT_OBJECT_SHOW, hidden.window);
    foreground = hidden.window; watcher.event(EVENT_SYSTEM_FOREGROUND, hidden.window); watcher.tick();
    check(routes.empty(), "pre-existing hidden Explorer remains retained when shown and activated");
    Frame fresh; watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    check(routes.size() == 1 && routes.back().source == fresh.band && routes.back().target == hidden.band,
          "newly shown baseline window can host later new windows");
  }
  {
    Frame existing, anchor; foreground = anchor.window; Watcher watcher;
    watcher.event(EVENT_OBJECT_DESTROY, anchor.window);
    watcher.event(EVENT_OBJECT_SHOW, anchor.window);
    watcher.event(EVENT_SYSTEM_FOREGROUND, anchor.window); watcher.tick();
    check(routes.empty(), "queued destroy does not reclassify a live same-identity anchor as a new source");
    Frame fresh; watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    check(routes.size() == 1 && routes.back().source == fresh.band && routes.back().target == anchor.band,
          "live anchor remains a destination after a stale destroy event");
  }
  {
    Frame first, second; foreground = first.window; Watcher watcher;
    watcher.event(EVENT_OBJECT_SHOW, first.window); watcher.event(EVENT_OBJECT_SHOW, second.window); watcher.tick();
    check(routes.empty(), "enable/restart leaves all pre-existing Explorer windows intact");
    foreground = second.window; watcher.event(EVENT_SYSTEM_FOREGROUND, second.window);
    Frame fresh(false); watcher.event(EVENT_OBJECT_CREATE, fresh.window); watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    check(routes.empty(), "new source waits for its tab band");
    fresh.attachBand(); watcher.tick();
    check(routes.size() == 1 && routes.back().source == fresh.band && routes.back().target == second.band && routes.back().helper == watcher.helper,
          "new window routes once into the last active existing Explorer frame");
    watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    check(routes.size() == 1, "duplicate show events never repeat an accepted import");
    Frame later; watcher.event(EVENT_OBJECT_SHOW, later.window); watcher.tick();
    check(routes.size() == 2 && routes.back().target != fresh.band, "accepted import source stays quarantined during its pending interval");
    elapsed += 26000; watcher.tick();
    check(routes.size() == 2, "a surviving source is never reimported after completion timeout");
    foreground = fresh.window; watcher.event(EVENT_SYSTEM_FOREGROUND, fresh.window);
    Frame afterTimeout; watcher.event(EVENT_OBJECT_SHOW, afterTimeout.window); watcher.tick();
    check(routes.size() == 3 && routes.back().target == fresh.band, "surviving source becomes a usable destination after its pending interval");
  }
  {
    Watcher watcher; Frame first;
    watcher.event(EVENT_OBJECT_CREATE, first.window); watcher.event(EVENT_OBJECT_SHOW, first.window); watcher.tick();
    check(routes.empty(), "first Explorer window remains as the initial tab host");
    Frame second; watcher.event(EVENT_OBJECT_SHOW, second.window); watcher.tick();
    check(routes.size() == 1 && routes.back().target == first.band, "later Explorer window can route into the first host");
  }
  {
    Frame existing; Watcher watcher; routeReply = 0;
    Frame first, second;
    watcher.event(EVENT_OBJECT_CREATE, first.window); watcher.event(EVENT_OBJECT_CREATE, second.window); watcher.tick();
    check(!routes.empty() && std::all_of(routes.begin(), routes.end(), [&](const Route &route) { return route.target == existing.band; }),
          "simultaneous pending sources never become each other's destinations");
    routeReply = 1; watcher.tick();
    check(routes.back().target == existing.band, "busy source retries against a retained destination");
  }
  {
    Frame existing; Watcher watcher; Frame fresh; std::unique_ptr<Frame> reentrant;
    duringSend = [&] {
      reentrant = std::make_unique<Frame>();
      watcher.event(EVENT_OBJECT_CREATE, reentrant->window);
    };
    watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    check(reentrant && routes.size() == 1 && routes[0].source == fresh.band, "new-window event can reenter synchronous route dispatch safely");
    watcher.tick();
    check(routes.size() == 2 && routes[1].source == reentrant->band && routes[1].target == existing.band,
          "reentrant candidate retains its identity without reimporting the original source");
  }
  {
    Frame existing; Watcher watcher; Frame fresh;
    const auto staleBand = fresh.band;
    duringCapability = [&] { RemovePropW(fresh.window, tabs::windowProperty); };
    watcher.event(EVENT_OBJECT_CREATE, fresh.window); watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    check(routes.empty(), "source band withdrawn during target capability probing is never sent a stale route");
    fresh.attachBand(); std::erase(visible, staleBand); DestroyWindow(staleBand);
    watcher.tick();
    check(fresh.band != staleBand && routes.size() == 1 && routes[0].source == fresh.band && routes[0].target == existing.band,
          "source routes only through its replacement published band after capability reentrancy");
  }
  {
    Frame existing(false); Watcher watcher; Frame fresh;
    watcher.event(EVENT_OBJECT_CREATE, fresh.window); watcher.tick();
    check(routes.empty(), "missing destination band keeps the source window");
    elapsed += 21000; watcher.tick(); existing.attachBand(); watcher.tick();
    check(routes.empty(), "expired candidates are never moved after a late band appears");
  }
  {
    Frame existing; Watcher watcher; Frame retained;
    routeReply = 2; watcher.event(EVENT_OBJECT_SHOW, retained.window); watcher.tick();
    foreground = retained.window; watcher.event(EVENT_SYSTEM_FOREGROUND, retained.window);
    routeReply = 1; Frame fresh; watcher.event(EVENT_OBJECT_SHOW, fresh.window); watcher.tick();
    check(routes.size() == 2 && routes.back().target == retained.band, "explicitly retained window is available for later tab routing");
  }
  {
    Frame existing; Watcher watcher; Frame invalid;
    validImage = false; watcher.event(EVENT_OBJECT_SHOW, invalid.window); watcher.tick();
    check(routes.empty(), "Explorer-looking classes in another image cannot be routed");
    validImage = true; foreign = invalid.window; watcher.event(EVENT_OBJECT_SHOW, invalid.window); watcher.tick();
    check(routes.empty(), "unverifiable process identity is rejected");
    foreign = nullptr; feature = false; watcher.event(EVENT_OBJECT_SHOW, invalid.window); watcher.tick();
    check(routes.empty(), "disabling routing prevents queued window transfers");
  }
}
void bypasses() {
  using tabs::routing::validPidl;
  using tabs::routing::bypassAdd;
  using tabs::routing::bypassCheck;
  using tabs::routing::bypassCancel;
  Frame first, second; Watcher watcher;
  const BYTE folder[]{4, 0, 1, 0, 0, 0}, other[]{4, 0, 2, 0, 0, 0}, invalid[]{1, 0, 0, 0};
  check(validPidl(folder, sizeof(folder)) && !validPidl(invalid, sizeof(invalid)), "test protocol identifiers are bounded PIDLs");
  check(!watcher.data(first.band, bypassAdd, invalid, sizeof(invalid)) && !watcher.data(first.window, bypassAdd, folder, sizeof(folder)),
        "malformed PIDLs and non-band senders cannot register exemptions");
  auto token = watcher.data(first.band, bypassAdd, folder, sizeof(folder));
  check(token != 0 && !watcher.data(second.band, bypassCheck, other, sizeof(other)), "bypass token matches only its requested folder");
  check(watcher.data(second.band, bypassCheck, folder, sizeof(folder)) != 0 && !watcher.data(second.band, bypassCheck, folder, sizeof(folder)),
        "matching explicit-window exemption is consumed exactly once");
  token = watcher.data(first.band, bypassAdd, folder, sizeof(folder));
  check(!watcher.data(second.band, bypassCancel, &token, sizeof(token)) && watcher.data(second.band, bypassCheck, folder, sizeof(folder)),
        "another band cannot cancel a registered explicit-window intent");
  token = watcher.data(first.band, bypassAdd, folder, sizeof(folder));
  check(watcher.data(first.band, bypassCancel, &token, sizeof(token)) && !watcher.data(second.band, bypassCheck, folder, sizeof(folder)),
        "registration owner can cancel a failed explicit launch");
  check(watcher.data(first.band, bypassAdd, folder, sizeof(folder)) != 0, "register expiring exemption");
  elapsed += 31000; watcher.tick();
  check(!watcher.data(second.band, bypassCheck, folder, sizeof(folder)), "expired exemption cannot exempt unrelated later windows");
  for (int i = 0; i < 16; ++i) check(watcher.data(first.band, bypassAdd, folder, sizeof(folder)) != 0, "register bounded exemption");
  check(!watcher.data(first.band, bypassAdd, folder, sizeof(folder)), "exemption list rejects overflow instead of growing without bound");
}
}
int main() {
  try {
    const auto instance = GetModuleHandleW(nullptr);
    for (auto name : {L"CabinetWClass", L"ExploreWClass", L"ShibaKit.ExplorerTabs.Band.v1"}) {
      WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = DefWindowProcW; cls.lpszClassName = name;
      check(RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "register isolated router window class");
    }
    holds(); lifecycle(); bypasses();
    std::cout << "PASS " << checks << " router checks: pre-show holds, capability, release, existing-window retention, new windows, readiness, MRU, quarantine, retries, bypass validation\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; return 1; }
}
