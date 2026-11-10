#include "tabs_routing.h"
#include <algorithm>
#include <string>

namespace tabs::routing {
namespace {
constexpr UINT eventMessage = WM_APP + 140;
constexpr UINT_PTR timerId = 40;
constexpr ULONGLONG candidateLifetime = 20000, bypassLifetime = 30000;
struct Frame {
  HWND window = nullptr;
  HWND capabilityBand = nullptr;
  DWORD process = 0;
  ULONGLONG deadline = 0, order = 0, serial = 0, capabilityRetry = 0, holdDeadline = 0;
  bool candidate = false, outbound = false, held = false, releaseSent = false;
  bool capabilityKnown = false, capable = false;
};
struct Bypass {
  ULONG_PTR token = 0;
  HWND sender = nullptr;
  DWORD process = 0;
  ULONGLONG deadline = 0;
  std::vector<BYTE> id;
};
struct State {
  HWND helper = nullptr;
  HWINEVENTHOOK windows = nullptr, foreground = nullptr;
  std::vector<Frame> frames;
  std::vector<Bypass> exemptions;
  ULONGLONG sequence = 0, generation = 0;
  ULONG_PTR token = 0;
  bool timer = false, saturated = false;
} state;
bool explorerClass(HWND window) {
  wchar_t name[64]{};
  return window && GetAncestor(window, GA_ROOT) == window && GetClassNameW(window, name, ARRAYSIZE(name)) &&
      (!wcscmp(name, L"CabinetWClass") || !wcscmp(name, L"ExploreWClass"));
}
DWORD processIntegrity(HANDLE process) {
  HANDLE token = nullptr;
  if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token)) return MAXDWORD;
  DWORD bytes = 0;
  GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
  BYTE data[256]{};
  BOOL valid = bytes && bytes <= sizeof(data) && GetTokenInformation(token, TokenIntegrityLevel, data, sizeof(data), &bytes);
  CloseHandle(token);
  if (!valid) return MAXDWORD;
  auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL *>(data)->Label.Sid;
  return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}
bool trustedFrame(HWND window, DWORD *identity = nullptr) {
  if (!explorerClass(window)) return false;
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
  if (!process) return false;
  wchar_t image[32768]{}, directory[MAX_PATH]{}; DWORD bytes = ARRAYSIZE(image);
  const bool named = QueryFullProcessImageNameW(process, 0, image, &bytes) &&
      GetWindowsDirectoryW(directory, ARRAYSIZE(directory)) &&
      !_wcsicmp(image, (std::wstring(directory) + L"\\explorer.exe").c_str());
  const DWORD targetIntegrity = named ? processIntegrity(process) : MAXDWORD;
  CloseHandle(process);
  const DWORD ownIntegrity = processIntegrity(GetCurrentProcess());
  if (!named || targetIntegrity == MAXDWORD || ownIntegrity == MAXDWORD || targetIntegrity != ownIntegrity) return false;
  if (identity) *identity = pid;
  return true;
}
bool trustedBand(HWND band, DWORD *identity = nullptr) {
  if (!bandWindow(band)) return false;
  DWORD pid = 0, framePid = 0;
  GetWindowThreadProcessId(band, &pid);
  if (!trustedFrame(GetAncestor(band, GA_ROOT), &framePid) || pid != framePid) return false;
  if (identity) *identity = pid;
  return true;
}
auto findFrame(HWND window) {
  return std::find_if(state.frames.begin(), state.frames.end(), [&](const Frame &frame) { return frame.window == window; });
}
bool sameFrame(const Frame &frame) {
  DWORD pid = 0;
  return IsWindow(frame.window) && GetWindowThreadProcessId(frame.window, &pid) && pid == frame.process;
}
void releaseGate(Frame &frame) {
  if (!frame.releaseSent && sameFrame(frame) && (frame.held || GetPropW(frame.window, gateProperty)))
    frame.releaseSent = PostMessageW(frame.window, releaseGateMessage, 0, 0) != FALSE;
}
void prune() {
  const auto now = GetTickCount64();
  std::erase_if(state.frames, [](const auto &frame) { return !sameFrame(frame); });
  std::erase_if(state.exemptions, [&](const auto &item) {
    DWORD pid = 0;
    return item.deadline <= now || !IsWindow(item.sender) ||
        !GetWindowThreadProcessId(item.sender, &pid) || pid != item.process;
  });
}
void schedule() {
  if (!state.helper) return;
  const bool pending = !state.exemptions.empty() ||
      std::any_of(state.frames.begin(), state.frames.end(), [](const auto &frame) { return frame.candidate || frame.outbound; });
  if (pending && !state.timer) state.timer = SetTimer(state.helper, timerId, 150, nullptr) != 0;
  else if (!pending && state.timer) { KillTimer(state.helper, timerId); state.timer = false; }
}
void observe(HWND window, bool existing, bool isForeground) {
  DWORD pid = 0;
  if (!trustedFrame(window, &pid)) return;
  auto found = findFrame(window);
  if (found != state.frames.end() && found->process != pid) { state.frames.erase(found); found = state.frames.end(); }
  if (found != state.frames.end()) {
    if (isForeground) found->order = ++state.sequence;
    return;
  }
  // A crowded session is retained conservatively rather than growing an
  // unbounded event queue or treating an untracked old window as disposable.
  if (state.saturated || state.frames.size() >= 128) { state.saturated = true; return; }
  const bool haveAnchor = std::any_of(state.frames.begin(), state.frames.end(), [](const Frame &frame) {
    return !frame.candidate && !frame.outbound;
  });
  Frame added; added.window = window; added.process = pid;
  added.serial = added.order = ++state.sequence;
  added.candidate = !existing && haveAnchor;
  added.deadline = GetTickCount64() + candidateLifetime;
  state.frames.push_back(added);
  schedule();
}
bool capableBand(const Frame &frame, HWND band) {
  auto found = findFrame(frame.window);
  if (found == state.frames.end() || found->serial != frame.serial) return false;
  if (found->capabilityBand == band) {
    if (found->capabilityKnown) return found->capable;
    if (found->capabilityRetry > GetTickCount64()) return false;
  }
  found->capabilityBand = band; found->capabilityKnown = false;
  found->capabilityRetry = GetTickCount64() + 1000;
  const auto generation = state.generation;
  DWORD_PTR reply = 0;
  const bool delivered = SendMessageTimeoutW(band, capabilityMessage, 0, 0, SMTO_ABORTIFHUNG, 100, &reply) != 0;
  if (state.generation != generation) return false;
  found = findFrame(frame.window);
  if (found == state.frames.end() || found->serial != frame.serial || found->capabilityBand != band ||
      GetPropW(frame.window, windowProperty) != band) return false;
  found->capabilityKnown = delivered; found->capable = delivered && reply >= 2;
  return found->capable;
}
HWND usableBand(const Frame &frame, bool forSource) {
  const bool gated = GetPropW(frame.window, gateProperty) != nullptr;
  if (!sameFrame(frame) || !trustedFrame(frame.window) || (!forSource && gated) ||
      (!IsWindowVisible(frame.window) && !(forSource && gated))) return nullptr;
  auto band = static_cast<HWND>(GetPropW(frame.window, windowProperty));
  if (!trustedBand(band) || (!IsWindowVisible(band) && !(forSource && gated))) return nullptr;
  if (!forSource && (!capableBand(frame, band) || !sameFrame(frame) || !IsWindowVisible(frame.window) ||
      GetPropW(frame.window, gateProperty) || GetPropW(frame.window, windowProperty) != band)) return nullptr;
  return band;
}
HWND targetBand(const Frame &source) {
  const auto generation = state.generation;
  auto targets = state.frames;
  std::sort(targets.begin(), targets.end(), [](const auto &a, const auto &b) { return a.order > b.order; });
  for (const auto &target : targets) {
    if (target.window == source.window || target.candidate || target.outbound) continue;
    auto band = usableBand(target, false);
    if (state.generation != generation) return nullptr;
    auto found = findFrame(target.window);
    if (band && found != state.frames.end() && found->serial == target.serial && !found->candidate && !found->outbound)
      return band;
  }
  return nullptr;
}
bool hold(HWND window) {
  if (IsWindowVisible(window) || !trustedFrame(window)) return false;
  observe(window, false, false);
  auto found = findFrame(window);
  if (found == state.frames.end() || !found->candidate || found->outbound || found->deadline <= GetTickCount64()) return false;
  const auto source = *found;
  const auto generation = state.generation;
  const auto target = targetBand(source);
  if (!target || state.generation != generation || !enabled() || IsWindowVisible(window) || !sameFrame(source)) return false;
  found = findFrame(window);
  if (found == state.frames.end() || found->serial != source.serial || !found->candidate || found->outbound ||
      found->deadline <= GetTickCount64()) return false;
  found->held = true; found->releaseSent = false; found->holdDeadline = found->deadline;
  schedule(); return true;
}
void tick() {
  prune();
  const auto now = GetTickCount64(), generation = state.generation;
  // A failed/abandoned transfer keeps its original frame. The source band has
  // a shorter completion deadline; eventually that surviving window is useful
  // as a destination again, without ever retrying its original transfer.
  for (auto &frame : state.frames) {
    if (frame.held && frame.holdDeadline <= now) { releaseGate(frame); frame.held = false; }
    if (frame.outbound && frame.deadline <= now) {
      releaseGate(frame); frame.held = false; frame.outbound = false;
    }
  }
  // Never retain references into frames across a cross-thread message. The
  // source can synchronously ask this helper about a bypass during dispatch.
  auto candidates = state.frames;
  std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) { return a.serial < b.serial; });
  for (const auto &source : candidates) {
    if (!source.candidate) continue;
    if (source.deadline <= now) {
      auto found = findFrame(source.window);
      if (found != state.frames.end() && found->serial == source.serial) {
        releaseGate(*found); found->held = false; found->candidate = false;
      }
      continue;
    }
    HWND sourceBand = usableBand(source, true);
    if (!sourceBand) continue;
    const auto target = targetBand(source);
    if (state.generation != generation) return;
    auto found = findFrame(source.window);
    if (!target || found == state.frames.end() || found->serial != source.serial || !found->candidate ||
        found->deadline <= GetTickCount64()) continue;
    const auto currentSource = *found;
    if (usableBand(currentSource, true) != sourceBand) continue;
    DWORD_PTR reply = 0;
    const bool delivered = SendMessageTimeoutW(sourceBand, routeMessage, reinterpret_cast<WPARAM>(target),
        reinterpret_cast<LPARAM>(state.helper), SMTO_ABORTIFHUNG, 1000, &reply) != 0;
    if (state.generation != generation) return;
    found = findFrame(source.window);
    if (found != state.frames.end() && found->serial == source.serial && (!delivered || reply == 1 || reply == 2)) {
      found->candidate = false;
      found->outbound = !delivered || reply == 1;
      if (found->outbound) found->deadline = GetTickCount64() + 25000;
      if (delivered && reply == 2) { releaseGate(*found); found->held = false; }
    }
    break; // At most one bounded cross-process dispatch per timer tick.
  }
  schedule();
}
void CALLBACK event(HWINEVENTHOOK, DWORD kind, HWND window, LONG object, LONG child, DWORD, DWORD) {
  if (!state.helper || !window) return;
  if (kind != EVENT_SYSTEM_FOREGROUND && (object != OBJID_WINDOW || child != CHILDID_SELF)) return;
  if (kind == EVENT_OBJECT_DESTROY) {
    if (findFrame(window) == state.frames.end()) return;
  } else if (!explorerClass(window)) return;
  PostMessageW(state.helper, eventMessage, kind, reinterpret_cast<LPARAM>(window));
}
LRESULT bypassRequest(HWND sender, const COPYDATASTRUCT *data) {
  DWORD pid = 0;
  if (!data || !trustedBand(sender, &pid)) return 0;
  prune();
  if (data->dwData == bypassCancel) {
    if (!data->lpData || data->cbData != sizeof(ULONG_PTR)) return 0;
    ULONG_PTR token = 0; memcpy(&token, data->lpData, sizeof(token));
    auto found = std::find_if(state.exemptions.begin(), state.exemptions.end(), [&](const Bypass &value) {
      return value.token == token && value.sender == sender && value.process == pid;
    });
    if (found == state.exemptions.end()) return 0;
    state.exemptions.erase(found); schedule(); return 1;
  }
  if (!validPidl(data->lpData, data->cbData)) return 0;
  if (data->dwData == bypassAdd) {
    if (state.exemptions.size() >= 16) return 0;
    Bypass item; item.sender = sender; item.process = pid; item.deadline = GetTickCount64() + bypassLifetime;
    if (!++state.token) ++state.token;
    item.token = state.token;
    auto bytes = static_cast<const BYTE *>(data->lpData); item.id.assign(bytes, bytes + data->cbData);
    state.exemptions.push_back(std::move(item)); schedule(); return state.token;
  }
  if (data->dwData != bypassCheck) return 0;
  // Copy the payload before comparing Shell identities; never retain WM_COPYDATA
  // storage, and tolerate a provider pumping messages while comparing PIDLs.
  auto bytes = static_cast<const BYTE *>(data->lpData);
  const std::vector<BYTE> id(bytes, bytes + data->cbData);
  const auto pending = state.exemptions;
  for (const auto &item : pending) {
    if (item.id != id && !ILIsEqual(reinterpret_cast<PCIDLIST_ABSOLUTE>(item.id.data()), reinterpret_cast<PCIDLIST_ABSOLUTE>(id.data()))) continue;
    auto found = std::find_if(state.exemptions.begin(), state.exemptions.end(), [&](const Bypass &value) { return value.token == item.token; });
    if (found == state.exemptions.end() || found->deadline <= GetTickCount64()) continue;
    state.exemptions.erase(found); schedule(); return 1;
  }
  return 0;
}
}
bool start(HWND helper) {
  stop();
  if (!enabled() || !IsWindow(helper)) return false;
  state.helper = helper;
  if (!state.token) state.token = static_cast<ULONG_PTR>(GetTickCount64());
  try {
    if (!EnumWindows([](HWND window, LPARAM) -> BOOL {
      try { observe(window, true, false); return TRUE; } catch (...) { return FALSE; }
    }, 0)) { stop(); return false; }
    observe(GetForegroundWindow(), true, true);
    state.windows = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, event, 0, 0, WINEVENT_OUTOFCONTEXT);
    state.foreground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, event, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (state.windows && state.foreground) return true;
  } catch (...) { }
  stop(); return false;
}
void stop() {
  ++state.generation;
  auto windowHook = state.windows, foregroundHook = state.foreground; auto helper = state.helper;
  state.windows = state.foreground = nullptr; state.helper = nullptr;
  if (windowHook) UnhookWinEvent(windowHook);
  if (foregroundHook) UnhookWinEvent(foregroundHook);
  if (helper && state.timer) KillTimer(helper, timerId);
  for (auto &frame : state.frames) releaseGate(frame);
  state.timer = state.saturated = false; state.frames.clear(); state.exemptions.clear();
}
bool message(HWND helper, UINT message, WPARAM w, LPARAM l, LRESULT &result) {
  const bool ours = message == holdMessage || message == eventMessage || (message == WM_TIMER && w == timerId) ||
      (message == WM_COPYDATA && l && (reinterpret_cast<const COPYDATASTRUCT *>(l)->dwData == bypassAdd ||
       reinterpret_cast<const COPYDATASTRUCT *>(l)->dwData == bypassCheck || reinterpret_cast<const COPYDATASTRUCT *>(l)->dwData == bypassCancel));
  if (!ours) return false;
  result = 0;
  if (!state.helper || state.helper != helper) return true;
  if (!enabled()) { stop(); return true; }
  try {
    if (message == holdMessage) result = hold(reinterpret_cast<HWND>(w)) ? TRUE : FALSE;
    else if (message == WM_COPYDATA) result = bypassRequest(reinterpret_cast<HWND>(w), reinterpret_cast<const COPYDATASTRUCT *>(l));
    else if (message == WM_TIMER) tick();
    else {
      auto window = reinterpret_cast<HWND>(l);
      if (w == EVENT_OBJECT_DESTROY) {
        // A queued event can outlive its window handle. Retain a live identity
        // rather than reclassifying an existing frame as a newly opened one.
        std::erase_if(state.frames, [&](const Frame &frame) { return frame.window == window && !sameFrame(frame); });
        schedule();
      }
      else if (w == EVENT_OBJECT_CREATE || w == EVENT_OBJECT_SHOW || w == EVENT_SYSTEM_FOREGROUND)
        observe(window, false, w == EVENT_SYSTEM_FOREGROUND);
    }
  } catch (...) { result = 0; }
  return true;
}
}
