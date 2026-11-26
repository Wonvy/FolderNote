#include "navigation_view.h"
#include "explorer_tabs.h"
#include "quick_folders.h"
#include "middle_click.h"
#include <commctrl.h>
#include <windowsx.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace viewnav {
namespace {
std::atomic<long> controllers{0};
std::atomic<long> queryWorkers{0};
std::atomic<UINT_PTR> nextToken{1};
UINT resultMessage() { static const UINT m = RegisterWindowMessageW(L"FolderNote.Navigation.QueryReady.v2"); return m; }
UINT refreshMessage() { static const UINT m = RegisterWindowMessageW(L"FolderNote.Navigation.RefreshView.v2"); return m; }
UINT settingsMessage() { static const UINT m = RegisterWindowMessageW(L"FolderNote.Navigation.SettingsChanged.v2"); return m; }
UINT shutdownMessage() { static const UINT m = RegisterWindowMessageW(L"FolderNote.Navigation.Shutdown.v2"); return m; }
constexpr wchar_t ownerProperty[] = L"FolderNote.Navigation.ViewOwner.v2";
constexpr wchar_t controllerProperty[] = L"FolderNote.Navigation.Controller.v3";
bool modifiers() {
  return ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT) |
           GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) |
           GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
}
struct Ready {
  navigation::Click click{};
  unsigned long long epoch{};
  navigation::Action action{};
};
struct Target {
  HWND receiver{};
  UINT_PTR token{};
  std::atomic<unsigned long long> epoch{1};
  std::atomic<bool> alive{true};
  std::mutex mutex;
  std::deque<Ready> ready;
  // Protected by mutex, including a retiring worker during rapid disable/enable.
  navigation::Click previous{};
  unsigned long long previousEpoch{};
  navigation::Action previousAction{};
};
struct Job {
  std::weak_ptr<Target> target;
  Ready data;
};
struct Queries {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Job> jobs;
  unsigned clients{};
  bool stopping{};
};
struct QueryStart { std::shared_ptr<Queries> queries; HMODULE module; };
std::mutex queriesMutex;
std::weak_ptr<Queries> currentQueries;

// UIA must run off Explorer's UI thread. The UI thread never waits for it.
bool blank(IUIAutomation *ui, const navigation::Click &click) {
  if (!ui || !IsWindowVisible(click.view) || GetForegroundWindow() != click.root)
    return false;
  HWND under = WindowFromPoint(click.point);
  if (under != click.view && !IsChild(click.view, under))
    return false;
  ComPtr<IUIAutomationElement> element;
  if (FAILED(ui->ElementFromPoint(click.point, &element)) || !element)
    return false;
  ComPtr<IUIAutomationTreeWalker> walker;
  // Empty-folder labels and passive layout panes can be returned as the hit
  // element on Win11. Walk only through passive children to the ItemsView;
  // crossing a file item, group, header or other interactive control is forbidden.
  for (unsigned depth = 0; element && depth < 8; ++depth) {
    CONTROLTYPEID type{};
    if (FAILED(element->get_CurrentControlType(&type)))
      return false;
    const bool surface = type == UIA_ListControlTypeId || type == UIA_DataGridControlTypeId ||
                         type == UIA_PaneControlTypeId;
    if (!surface && type != UIA_TextControlTypeId && type != UIA_CustomControlTypeId)
      return false;
    BSTR cls = nullptr, id = nullptr;
    element->get_CurrentClassName(&cls);
    element->get_CurrentAutomationId(&id);
    const bool contents = surface &&
        ((cls && (!wcscmp(cls, L"UIItemsView") || !wcscmp(cls, L"SysListView32"))) ||
         (id && !wcscmp(id, L"ItemsView")));
    SysFreeString(cls);
    SysFreeString(id);
    if (contents)
      return true;
    if (!walker && FAILED(ui->get_RawViewWalker(&walker)))
      return false;
    ComPtr<IUIAutomationElement> parent;
    if (!walker || FAILED(walker->GetParentElement(element.Get(), &parent)))
      return false;
    element = std::move(parent);
  }
  return false;
}
DWORD WINAPI queryThread(void *argument) {
  ++queryWorkers;
  HMODULE keepAlive = nullptr;
  // The creating thread took this reference before CreateThread, so the DLL
  // cannot unload while a query is completing after a window has closed.
  {
    std::unique_ptr<QueryStart> start(static_cast<QueryStart *>(argument));
    keepAlive = start->module;
    auto queries = start->queries;
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(init)) {
      try {
        ComPtr<IUIAutomation> ui;
        CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui));
        if (!ui)
          CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui));
        ComPtr<IUIAutomation2> options;
        if (ui && SUCCEEDED(ui.As(&options))) {
          options->put_ConnectionTimeout(150);
          options->put_TransactionTimeout(150);
        }
        for (;;) {
          Job job;
          {
            std::unique_lock lock(queries->mutex);
            queries->wake.wait(lock, [&] { return queries->stopping || !queries->jobs.empty(); });
            if (queries->stopping)
              break;
            job = queries->jobs.front();
            queries->jobs.pop_front();
          }
          auto target = job.target.lock();
          if (!target || !target->alive || target->epoch != job.data.epoch)
            continue;
          auto &c = job.data.click;
          if (DWORD(GetTickCount() - c.time) > 500) {
            std::lock_guard lock(target->mutex);
            target->previous = {};
            continue;
          }
          c.blank = blank(ui.Get(), c);
          std::lock_guard targetLock(target->mutex);
          if (!target->alive || target->epoch != job.data.epoch ||
              DWORD(GetTickCount() - c.time) > 500) {
            target->previous = {};
            continue;
          }
          if (target->previousEpoch == job.data.epoch &&
              target->previousAction == job.data.action &&
              navigation::isDoubleClick(target->previous, c, GetDoubleClickTime(),
                                       GetSystemMetrics(SM_CXDOUBLECLK),
                                       GetSystemMetrics(SM_CYDOUBLECLK))) {
            target->previous = {};
            target->ready.clear();
            target->ready.push_back(job.data);
            PostMessageW(target->receiver, resultMessage(), target->token, 0);
          } else {
            target->previous = c;
            target->previousEpoch = job.data.epoch;
            target->previousAction = job.data.action;
          }
        }
      } catch (...) { /* Disable queries on allocation/COM wrapper failure; never crash Explorer. */ }
      CoUninitialize();
    }
  }
  --queryWorkers;
  FreeLibraryAndExitThread(keepAlive, 0);
}
std::shared_ptr<Queries> acquireQueries() {
  std::lock_guard lock(queriesMutex);
  auto queries = currentQueries.lock();
  if (queries && !queries->stopping) {
    ++queries->clients;
    return queries;
  }
  queries = std::make_shared<Queries>();
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                         reinterpret_cast<LPCWSTR>(&queryThread), &module))
    return {};
  auto start = new (std::nothrow) QueryStart{queries, module};
  if (!start) {
    FreeLibrary(module);
    return {};
  }
  HANDLE thread = CreateThread(nullptr, 0, queryThread, start, 0, nullptr);
  if (!thread) {
    delete start;
    FreeLibrary(module);
    return {};
  }
  CloseHandle(thread);
  queries->clients = 1;
  currentQueries = queries;
  return queries;
}
void releaseQueries(std::shared_ptr<Queries> &queries) {
  if (!queries)
    return;
  {
    std::lock_guard lock(queriesMutex);
    if (--queries->clients == 0) {
      std::lock_guard queueLock(queries->mutex);
      queries->stopping = true;
      queries->jobs.clear();
      queries->wake.notify_one();
    }
  }
  queries.reset();
}
} // namespace

struct Controller {
  HWND host{}, frame{}, view{};
  DWORD thread{};
  ComPtr<IShellBrowser> browser;
  std::vector<HWND> surfaces;
  std::shared_ptr<Target> target;
  std::shared_ptr<Queries> queries;
  POINT down{};
  HWND pressed{};
  bool dragged{}, stopped{}, refreshPosted{}, detached{};
  unsigned callbacks{};
  navigation::Action action{};
  HWND middlePressed{};
  POINT middleDown{};
  bool middleDragged{};
  std::wstring lastVisited;
};
// Shell navigation can pump messages and disconnect the BHO reentrantly.
// Keep the controller alive until every subclass callback has returned.
struct CallbackGuard {
  Controller *c;
  explicit CallbackGuard(Controller *value) : c(value) { ++c->callbacks; }
  ~CallbackGuard() {
    if (--c->callbacks == 0 && c->detached) {
      delete c;
      --controllers;
    }
  }
};
static LRESULT CALLBACK frameProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static LRESULT CALLBACK surfaceProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
static void resetDoubleClick(Controller *c) {
  c->pressed = nullptr;
  if (c->target)
    ++c->target->epoch;
}
static void resetGesture(Controller *c) {
  c->middlePressed = nullptr;
  resetDoubleClick(c);
}
static void clearView(Controller *c) {
  resetGesture(c);
  for (auto h : c->surfaces) {
    if (GetPropW(h, ownerProperty) == c) {
      RemoveWindowSubclass(h, surfaceProc, reinterpret_cast<UINT_PTR>(c));
      RemovePropW(h, ownerProperty);
    }
  }
  c->surfaces.clear();
  c->view = nullptr;
}
static void requestRefresh(Controller *c) {
  if (!c->refreshPosted && c->host && !c->stopped && !c->detached) {
    c->refreshPosted = PostMessageW(c->host, refreshMessage(), c->target->token, 0) != FALSE;
  }
}
static void addSurface(Controller *c, HWND h) {
  if (GetWindowThreadProcessId(h, nullptr) != c->thread || GetPropW(h, ownerProperty))
    return;
  // Allocate bookkeeping before installing a callback with a raw controller pointer.
  c->surfaces.push_back(h);
  if (!SetPropW(h, ownerProperty, c)) {
    c->surfaces.pop_back();
    return;
  }
  if (!SetWindowSubclass(h, surfaceProc, reinterpret_cast<UINT_PTR>(c), reinterpret_cast<DWORD_PTR>(c))) {
    RemovePropW(h, ownerProperty);
    c->surfaces.pop_back();
  }
}
void refresh(Controller *c, IShellBrowser *browser) {
  if (!c || c->detached || GetCurrentThreadId() != c->thread || !c->host)
    return;
  CallbackGuard guard(c);
  if (browser)
    c->browser = browser;
  if (c->detached || !c->host)
    return;
  c->action = navigation::configuredAction();
  const bool quickEnabled = navigation::quickEnabled();
  const bool tabsEnabled = navigation::tabsSupported() && navigation::tabsEnabled();
  if (c->stopped || (c->action == navigation::Action::Off && !quickEnabled && !tabsEnabled)) {
    clearView(c);
    c->lastVisited.clear();
    releaseQueries(c->queries);
    return;
  }
  ComPtr<IShellView> view;
  HWND handle = nullptr;
  auto currentBrowser = c->browser;
  if (!currentBrowser)
    return;
  HRESULT query = currentBrowser->QueryActiveShellView(&view);
  if (c->detached || !c->host || c->stopped)
    return;
  HRESULT window = SUCCEEDED(query) && view ? view->GetWindow(&handle) : E_FAIL;
  if (c->detached || !c->host || c->stopped)
    return;
  if (FAILED(window) || !handle || GetAncestor(handle, GA_ROOT) != c->frame ||
      (handle != c->host && !IsChild(c->host, handle)) ||
      GetWindowThreadProcessId(handle, nullptr) != c->thread) {
    clearView(c);
    return;
  }
  if (handle != c->view) {
    clearView(c);
    c->view = handle;
  }
  if (c->action != navigation::Action::Off) {
    if (!c->queries) c->queries = acquireQueries();
  } else releaseQueries(c->queries);
  if (quickEnabled) {
    auto path = quick::viewPath(view.Get());
    if (c->detached || !c->host || c->stopped) return;
    if (!path.empty() && !quick::same(path, c->lastVisited)) {
      c->lastVisited = path;
      quick::visitAsync(path);
    }
  } else c->lastVisited.clear();
  // Also observe the Shell view container to discover replacement list HWNDs.
  addSurface(c, handle);
  EnumChildWindows(handle, [](HWND h, LPARAM parameter) -> BOOL {
    auto controller = reinterpret_cast<Controller *>(parameter);
    wchar_t cls[128]{};
    GetClassNameW(h, cls, 128);
    if (!wcscmp(cls, L"DirectUIHWND") || !wcscmp(cls, L"SysListView32") ||
        !wcscmp(cls, L"DUIViewWndClassName"))
      addSurface(controller, h);
    return TRUE;
  }, reinterpret_cast<LPARAM>(c));
}
HRESULT browse(IShellBrowser *browser, HWND expectedView, navigation::Action action) {
  if (!browser || !expectedView ||
      (action != navigation::Action::Parent && action != navigation::Action::Back))
    return E_INVALIDARG;
  ComPtr<IShellView> active;
  HWND handle = nullptr;
  HRESULT hr = browser->QueryActiveShellView(&active);
  if (FAILED(hr) || !active)
    return FAILED(hr) ? hr : E_FAIL;
  hr = active->GetWindow(&handle);
  if (FAILED(hr) || handle != expectedView)
    return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
  return browser->BrowseObject(nullptr, SBSP_SAMEBROWSER |
      (action == navigation::Action::Parent ? SBSP_PARENT : SBSP_NAVIGATEBACK));
}
static void consume(Controller *c) {
  std::deque<Ready> ready;
  {
    std::lock_guard lock(c->target->mutex);
    ready.swap(c->target->ready);
  }
  for (const auto &r : ready) {
    if (c->detached || c->stopped || r.epoch != c->target->epoch || r.click.view != c->view ||
        r.action != navigation::configuredAction() || r.action == navigation::Action::Off ||
        GetForegroundWindow() != c->frame || !IsWindowVisible(c->view) || modifiers() ||
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || DWORD(GetTickCount() - r.click.time) > 500)
      continue;
    // Navigation can synchronously recreate the view. Do not use its old HWND afterward.
    resetGesture(c);
    browse(c->browser.Get(), c->view, r.action);
    requestRefresh(c);
  }
}
static bool forwardTabWheel(Controller *c, WPARAM keys, LPARAM point) {
  if (c->detached || c->stopped || !c->frame || !navigation::tabsSupported() || !navigation::tabsEnabled())
    return false;
  const auto band = static_cast<HWND>(GetPropW(c->frame, tabs::windowProperty));
  wchar_t name[64]{};
  if (!IsWindow(band) || GetWindowThreadProcessId(band, nullptr) != c->thread ||
      !IsChild(c->frame, band) || GetAncestor(band, GA_ROOT) != c->frame ||
      !IsWindowVisible(band) || !IsWindowEnabled(band) || !IsWindowEnabled(c->frame) ||
      !GetClassNameW(band, name, ARRAYSIZE(name)) || wcscmp(name, L"FolderNote.ExplorerTabs.Band.v1"))
    return false;
  // WM_MOUSEWHEEL already supplies a screen point. Only the band's actual
  // hit-test may consume it; otherwise native file-view scrolling continues.
  return SendMessageW(band, tabs::wheelInputMessage, keys, point) == TRUE;
}
static LRESULT CALLBACK frameProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto c = reinterpret_cast<Controller *>(data);
  CallbackGuard guard(c);
  try {
    if (m == WM_MOUSEWHEEL && forwardTabWheel(c, w, l)) { resetGesture(c); return TRUE; }
    if (m == settingsMessage()) {
      c->stopped = false;
      resetGesture(c);
      refresh(c, nullptr);
    } else if (m == shutdownMessage()) {
      c->stopped = true;
      clearView(c);
      releaseQueries(c->queries);
    } else if (m == refreshMessage() && w == c->target->token) {
      c->refreshPosted = false;
      refresh(c, nullptr);
    } else if (m == resultMessage() && w == c->target->token) {
      consume(c);
    } else if (m == WM_ACTIVATE || m == WM_SHOWWINDOW ||
               (m == WM_PARENTNOTIFY && (LOWORD(w) == WM_CREATE || LOWORD(w) == WM_DESTROY))) {
      resetGesture(c);
      requestRefresh(c);
    } else if (m == WM_NCDESTROY) {
      c->target->alive = false;
      clearView(c);
      releaseQueries(c->queries);
      c->frame = nullptr;
      c->host = nullptr;
      RemovePropW(h, controllerProperty);
      RemoveWindowSubclass(h, frameProc, id);
    }
  } catch (...) {
    // A failing optional feature must never unwind through Explorer's window proc.
    c->stopped = true;
    resetGesture(c);
  }
  return DefSubclassProc(h, m, w, l);
}
static LRESULT CALLBACK surfaceProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
  auto c = reinterpret_cast<Controller *>(data);
  CallbackGuard guard(c);
  try {
    if (m == WM_MOUSEWHEEL && forwardTabWheel(c, w, l)) { resetGesture(c); return TRUE; }
    if (m == WM_NCDESTROY) {
      RemoveWindowSubclass(h, surfaceProc, id);
      RemovePropW(h, ownerProperty);
      std::erase(c->surfaces, h);
      resetGesture(c);
      requestRefresh(c);
    } else if ((m == WM_PARENTNOTIFY && (LOWORD(w) == WM_CREATE || LOWORD(w) == WM_DESTROY)) ||
               m == WM_SHOWWINDOW) {
      resetGesture(c);
      requestRefresh(c);
    } else if ((m == WM_MBUTTONDOWN || m == WM_MBUTTONDBLCLK) && !c->stopped && navigation::quickEnabled() &&
               !navigation::dialogQuickEnabled() && !modifiers()) {
      resetGesture(c);
      c->middlePressed = h;
      c->middleDown = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ClientToScreen(h, &c->middleDown);
      c->middleDragged = false;
      return 0;
    } else if (m == WM_MBUTTONUP && c->middlePressed == h) {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(h, &point);
      bool valid = !c->middleDragged && !c->stopped && !modifiers() && navigation::quickEnabled() &&
                   !navigation::dialogQuickEnabled() && GetForegroundWindow() == c->frame && IsWindowVisible(c->view) &&
                   quick::middleClickWithin(c->middleDown, point);
      resetGesture(c);
      if (valid) {
        try { quick::launchMenu(c->frame, c->view, point); } catch (...) {}
      }
      return 0;
    } else if (m == WM_MOUSEMOVE && c->middlePressed == h) {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)}; ClientToScreen(h, &point);
      if (!quick::middleClickWithin(c->middleDown, point)) c->middleDragged = true;
    } else if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
      if (!c->stopped && navigation::configuredAction() != navigation::Action::Off && !modifiers()) {
        c->pressed = h;
        c->down = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &c->down);
        c->dragged = false;
      } else
        resetGesture(c);
    } else if (m == WM_MOUSEMOVE && c->pressed == h) {
      POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      ClientToScreen(h, &p);
      if (std::abs(p.x - c->down.x) > GetSystemMetrics(SM_CXDRAG) / 2 ||
          std::abs(p.y - c->down.y) > GetSystemMetrics(SM_CYDRAG) / 2)
        c->dragged = true;
    } else if (m == WM_LBUTTONUP) {
      if (c->pressed == h && !c->dragged && !modifiers() && c->queries &&
          GetForegroundWindow() == c->frame && IsWindowVisible(c->view)) {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(h, &p);
        Job job{c->target, {{p, GetTickCount(), c->frame, c->view, false},
                           c->target->epoch.load(), navigation::configuredAction()}};
        std::lock_guard lock(c->queries->mutex);
        if (c->queries->jobs.size() >= 32) {
          resetGesture(c);
        } else {
          c->queries->jobs.push_back(job);
          c->queries->wake.notify_one();
        }
        c->pressed = nullptr;
      } else
        resetGesture(c);
    } else if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) {
      resetDoubleClick(c);
    } else if (m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN || m == WM_MBUTTONDBLCLK ||
               m == WM_CANCELMODE || m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
      resetGesture(c);
    }
  } catch (...) {
    c->stopped = true;
    resetGesture(c);
  }
  // Never eat Explorer input; a blank double-click has no native navigation action.
  return DefSubclassProc(h, m, w, l);
}
HWND attachmentWindow(HWND frame, HWND browserWindow, HWND view) {
  if (!IsWindow(frame) || GetAncestor(frame, GA_ROOT) != frame)
    return nullptr;
  auto sameThread = [](HWND h) {
    return IsWindow(h) && GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId();
  };
  if (view) {
    if (!sameThread(view) || GetAncestor(view, GA_ROOT) != frame)
      return nullptr;
    // Each native Win11 tab is a distinct browser even though IWebBrowser2::HWND
    // can report the shared frame. Keep those attachment/lifetime scopes distinct.
    for (HWND h = view; h && h != frame; h = GetParent(h)) {
      wchar_t name[64]{};
      GetClassNameW(h, name, ARRAYSIZE(name));
      if (!wcscmp(name, L"ShellTabWindowClass") && sameThread(h))
        return h;
    }
  }
  if (sameThread(browserWindow) && GetAncestor(browserWindow, GA_ROOT) == frame &&
      (!view || browserWindow == view || IsChild(browserWindow, view)))
    return browserWindow;
  if (sameThread(frame))
    return frame;
  // A different-thread frame cannot be subclassed. Use the highest ancestor of
  // this view that still belongs to the BHO's apartment instead.
  HWND host = view;
  for (HWND parent = host ? GetParent(host) : nullptr;
       parent && parent != frame && sameThread(parent); parent = GetParent(host))
    host = parent;
  return host;
}
Controller *attach(HWND host, IShellBrowser *browser) {
  if (!host || !browser || GetWindowThreadProcessId(host, nullptr) != GetCurrentThreadId() ||
      GetPropW(host, controllerProperty))
    return nullptr;
  auto c = std::make_unique<Controller>();
  c->host = host;
  c->frame = GetAncestor(host, GA_ROOT);
  c->thread = GetCurrentThreadId();
  c->target = std::make_shared<Target>();
  c->target->receiver = host;
  c->target->token = nextToken++;
  if (!SetPropW(host, controllerProperty, c.get()))
    return nullptr;
  if (!SetWindowSubclass(host, frameProc, reinterpret_cast<UINT_PTR>(c.get()), reinterpret_cast<DWORD_PTR>(c.get()))) {
    RemovePropW(host, controllerProperty);
    return nullptr;
  }
  ++controllers;
  auto result = c.release();
  CallbackGuard guard(result);
  try { refresh(result, browser); }
  catch (...) { detach(result); return nullptr; }
  return result->detached ? nullptr : result;
}
void detach(Controller *c) {
  if (!c || c->detached || c->thread != GetCurrentThreadId())
    return;
  c->detached = true;
  c->target->alive = false;
  clearView(c);
  if (c->host) {
    RemoveWindowSubclass(c->host, frameProc, reinterpret_cast<UINT_PTR>(c));
    RemovePropW(c->host, controllerProperty);
  }
  releaseQueries(c->queries);
  if (!c->callbacks) {
    delete c;
    --controllers;
  }
}
long activeControllers() { return controllers.load(); }
long activeQueryWorkers() { return queryWorkers.load(); }
bool hasView(const Controller *c) { return c && !c->detached && c->view && !c->surfaces.empty(); }
} // namespace viewnav
