#include "quick_find_external.h"
#include "quick_find.h"
#include "quick_find_popup.h"
#include "quick_dialog_history.h"
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <chrono>
#include <shellapi.h>
#include <filesystem>

namespace quick_find::external {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto settingsKey = L"Software\\FolderNote";
constexpr auto settingName = L"PinyinQuickFind";
struct Input { HWND root, focus; DWORD process; wchar_t character; ULONGLONG time; unsigned long long epoch; bool command = false; };
std::atomic<HWND> currentTarget{};
std::atomic<HWND> candidateWindow{};
std::atomic<bool> candidatePending{false};
std::atomic<unsigned long long> epoch{1};
std::atomic<bool> stopping{true};
std::mutex mutex;
std::condition_variable wake;
std::deque<Input> pending;
std::thread worker;

bool fileSurface(HWND focus) {
  wchar_t type[80]{};
  if (!focus || !GetClassNameW(focus, type, ARRAYSIZE(type))) return false;
  return !wcscmp(type, L"DirectUIHWND") || !wcscmp(type, L"SysListView32") ||
      !wcscmp(type, L"UIItemsView") || !wcscmp(type, L"SHELLDLL_DefView");
}
bool current(const Input &input) {
  DWORD process = 0;
  const auto thread = GetWindowThreadProcessId(input.root, &process);
  GUITHREADINFO info{sizeof(info)};
  return !stopping && input.epoch == epoch && input.root == currentTarget &&
      input.root == GetForegroundWindow() && process == input.process && thread &&
      GetGUIThreadInfo(thread, &info) && info.hwndFocus == input.focus &&
      !(info.flags & (GUI_INMENUMODE | GUI_INMOVESIZE)) && IsWindowVisible(input.focus);
}
ComPtr<IUIAutomationElement> focusedList(IUIAutomation *ui, const Input &input) {
  if (!current(input)) return {};
  ComPtr<IUIAutomationElement> element;
  ComPtr<IUIAutomationTreeWalker> walker;
  if (FAILED(ui->GetFocusedElement(&element)) || FAILED(ui->get_ControlViewWalker(&walker)) || !walker) return {};
  for (int depth = 0; element && depth < 8; ++depth) {
    CONTROLTYPEID type = 0;
    int process = 0;
    if (FAILED(element->get_CurrentControlType(&type)) || FAILED(element->get_CurrentProcessId(&process)) ||
        process != static_cast<int>(input.process)) return {};
    // In-place rename and filename/address edits must never become list searches.
    if (type == UIA_EditControlTypeId || type == UIA_ComboBoxControlTypeId || type == UIA_ButtonControlTypeId ||
        type == UIA_TreeItemControlTypeId || type == UIA_TreeControlTypeId) return {};
    if (type == UIA_ListControlTypeId || type == UIA_DataGridControlTypeId) {
      BSTR cls = nullptr;
      element->get_CurrentClassName(&cls);
      const bool fileList = cls && (!wcscmp(cls, L"UIItemsView") || !wcscmp(cls, L"SysListView32"));
      SysFreeString(cls);
      return fileList ? element : ComPtr<IUIAutomationElement>{};
    }
    ComPtr<IUIAutomationElement> parent;
    if (FAILED(walker->GetParentElement(element.Get(), &parent))) return {};
    element = std::move(parent);
  }
  return {};
}
struct Item { ComPtr<IUIAutomationElement> element; };
struct Contents { std::vector<Item> items; std::vector<Name> names; std::vector<std::wstring> labels; };
Contents readItems(IUIAutomation *ui, IUIAutomationElement *list, const Input &input) {
  Contents result;
  const auto append = [&](ComPtr<IUIAutomationElement> item) {
    BSTR name = nullptr;
    if (SUCCEEDED(item->get_CurrentName(&name)) && name) {
      result.names.emplace_back(std::wstring_view(name, SysStringLen(name)));
      result.labels.emplace_back(name, SysStringLen(name));
      result.items.push_back({std::move(item)});
    }
    SysFreeString(name);
  };
  ComPtr<IUIAutomationItemContainerPattern> container;
  if (SUCCEEDED(list->GetCurrentPatternAs(UIA_ItemContainerPatternId, IID_PPV_ARGS(&container))) && container) {
    ComPtr<IUIAutomationElement> previous;
    VARIANT empty{};
    for (int i = 0; i < 20000 && current(input); ++i) {
      ComPtr<IUIAutomationElement> item;
      if (FAILED(container->FindItemByProperty(previous.Get(), 0, empty, &item)) || !item) break;
      BOOL same = FALSE;
      if (previous && SUCCEEDED(ui->CompareElements(previous.Get(), item.Get(), &same)) && same) break;
      previous = item;
      append(std::move(item));
      if (GetTickCount64() - input.time > 8000) break;
    }
  } else {
    VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> items;
    if (SUCCEEDED(ui->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)) &&
        condition && SUCCEEDED(list->FindAll(TreeScope_Descendants, condition.Get(), &items)) && items) {
      int count = 0;
      items->get_Length(&count);
      for (int i = 0; i < count && i < 20000 && current(input); ++i) {
        ComPtr<IUIAutomationElement> item;
        if (SUCCEEDED(items->GetElement(i, &item)) && item) append(std::move(item));
      }
    }
  }
  return result;
}
void locate(IUIAutomation *ui, const Input &input, IUIAutomationElement *list,
            const Contents &contents, int row) {
  if (row < 0 || size_t(row) >= contents.items.size() || !current(input)) return;
  auto focused = focusedList(ui, input);
  BOOL same = FALSE;
  if (!focused || FAILED(ui->CompareElements(list, focused.Get(), &same)) || !same) return;
  const auto item = contents.items[row].element;
  BSTR name = nullptr; item->get_CurrentName(&name);
  const bool unchanged = name && contents.labels[row] == name;
  SysFreeString(name);
  if (!unchanged) return;
  // Shell list providers often leave ItemType empty. Verify the real directory
  // instead, and reject ambiguous hidden-extension names before invoking.
  const auto parent = quick::dialog::currentPath(input.root);
  const auto path = std::filesystem::path(parent) / contents.labels[row];
  const auto attributes = parent.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
  bool folder = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
  if (folder) {
    WIN32_FIND_DATAW entry{};
    const auto search = FindFirstFileW((path.native() + L".*").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
      int checked = 0;
      do {
        if (++checked > 256) { folder = false; break; }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        SHFILEINFOW info{};
        const auto other = std::filesystem::path(parent) / entry.cFileName;
        if (!SHGetFileInfoW(other.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) ||
            contents.labels[row] == info.szDisplayName) { folder = false; break; }
      } while (FindNextFileW(search, &entry));
      FindClose(search);
    }
  }
  ComPtr<IUIAutomationVirtualizedItemPattern> virtualized;
  if (SUCCEEDED(item->GetCurrentPatternAs(UIA_VirtualizedItemPatternId, IID_PPV_ARGS(&virtualized))) && virtualized)
    virtualized->Realize();
  if (!current(input)) return;
  ComPtr<IUIAutomationSelectionItemPattern> selection;
  if (FAILED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection))) || !selection) return;
  if (FAILED(selection->Select()) || !current(input)) return;
  ComPtr<IUIAutomationScrollItemPattern> scroll;
  if (SUCCEEDED(item->GetCurrentPatternAs(UIA_ScrollItemPatternId, IID_PPV_ARGS(&scroll))) && scroll) scroll->ScrollIntoView();
  if (folder && current(input)) {
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke) { invoke->Invoke(); return; }
    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(item->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy))) && legacy) {
      legacy->DoDefaultAction(); return;
    }
  }
  if (current(input)) item->SetFocus();
}
void run() {
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return;
  {
    ComPtr<IUIAutomation> ui;
    CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui));
    ComPtr<IUIAutomation2> options;
    if (ui && SUCCEEDED(ui.As(&options))) {
      options->put_ConnectionTimeout(200); options->put_TransactionTimeout(200);
    }
    Popup popup;
    Contents contents;
    ComPtr<IUIAutomationElement> list;
    unsigned long long previousEpoch = 0;
    while (ui && !stopping) {
      const bool wasOpen = popup.active();
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
      if (previousEpoch != epoch) popup.close();
      if (wasOpen && !popup.active()) candidatePending = false;
      candidateWindow = popup.active() ? popup.handle() : nullptr;
      std::deque<Input> batch;
      {
        std::unique_lock lock(mutex);
        // Keep the modeless window responsive, including mouse confirmation.
        wake.wait_for(lock, std::chrono::milliseconds(20), [] { return stopping || !pending.empty(); });
        if (stopping) break;
        batch.swap(pending);
      }
      if (batch.empty()) continue;
      try {
        const auto input = batch.front();
        if (!current(input)) { popup.close(); candidatePending = false; continue; }
        if (!popup.active()) {
          if (input.command) { candidatePending = false; continue; }
          list = focusedList(ui.Get(), input);
          if (!list) { candidatePending = false; continue; }
          contents = readItems(ui.Get(), list.Get(), input);
          if (!current(input)) { candidatePending = false; continue; }
          previousEpoch = input.epoch;
          const auto parent = quick::dialog::currentPath(input.root);
          std::vector<CandidateIcon> icons;
          for (const auto &label : contents.labels) icons.emplace_back(quick::icons::Source::child(parent, label));
          if (!current(input)) { candidatePending = false; continue; }
          popup.begin(input.focus, contents.labels, [&, input](int row) { locate(ui.Get(), input, list.Get(), contents, row); }, std::move(icons));
        }
        // Include typing that arrived while virtualized items were enumerated.
        {
          std::lock_guard lock(mutex);
          while (!pending.empty() && pending.front().epoch == input.epoch && pending.front().focus == input.focus) {
            batch.push_back(pending.front()); pending.pop_front();
          }
        }
        for (const auto &letter : batch) {
          if (letter.epoch != input.epoch || letter.focus != input.focus) continue;
          if (letter.command) popup.key(letter.character);
          else popup.character(letter.character);
        }
        candidateWindow = popup.active() ? popup.handle() : nullptr;
        candidatePending = popup.active();
      } catch (...) { popup.close(); candidateWindow = nullptr; candidatePending = false; }
    }
    candidateWindow = nullptr;
  }
  CoUninitialize();
}
}
bool enabled() {
  DWORD value = 0, size = sizeof(value);
  return RegGetValueW(HKEY_CURRENT_USER, settingsKey, settingName, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
}
void saveEnabled(bool value) {
  const DWORD flag = value ? 1 : 0;
  if (RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, settingName, REG_DWORD, &flag, sizeof(flag)) != ERROR_SUCCESS)
    throw std::runtime_error("could not save quick find preference");
}
bool start() {
  if (worker.joinable()) return true;
  try { stopping = false; worker = std::thread(run); return true; }
  catch (...) { stopping = true; return false; }
}
void cancel() { ++epoch; candidatePending = false; std::lock_guard lock(mutex); pending.clear(); }
void target(HWND window) { if (currentTarget.exchange(window) != window) cancel(); }
void stop() {
  currentTarget = nullptr; stopping = true; cancel(); wake.notify_one();
  if (worker.joinable()) worker.join();
}
bool keyboard(WPARAM message, const KBDLLHOOKSTRUCT &key) {
  static bool captured[256]{}; // Only accessed by the keyboard hook thread.
  if (message == WM_KEYUP || message == WM_SYSKEYUP) {
    if (key.vkCode < 256 && captured[key.vkCode]) { captured[key.vkCode] = false; return true; }
    return false;
  }
  if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return false;
  if (key.vkCode == VK_SHIFT || key.vkCode == VK_LSHIFT || key.vkCode == VK_RSHIFT || key.vkCode == VK_CAPITAL) return false;
  const auto root = currentTarget.load();
  if (!root || root != GetForegroundWindow() || stopping) return false;
  const bool letter = (key.vkCode >= 'A' && key.vkCode <= 'Z') || (key.vkCode >= '0' && key.vkCode <= '9');
  const auto popup = candidateWindow.load();
  const bool choosing = candidatePending || (popup && IsWindowVisible(popup));
  const bool command = choosing && (key.vkCode == VK_RETURN || key.vkCode == VK_ESCAPE || key.vkCode == VK_BACK ||
      key.vkCode == VK_UP || key.vkCode == VK_DOWN || key.vkCode == VK_HOME || key.vkCode == VK_END ||
      key.vkCode == VK_PRIOR || key.vkCode == VK_NEXT);
  if ((!letter && !command) || (key.flags & LLKHF_ALTDOWN) ||
      ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)) {
    cancel(); return false;
  }
  DWORD process = 0;
  const auto thread = GetWindowThreadProcessId(root, &process);
  GUITHREADINFO info{sizeof(info)};
  if (!thread || !GetGUIThreadInfo(thread, &info) || !fileSurface(info.hwndFocus) ||
      (info.flags & (GUI_INMENUMODE | GUI_INMOVESIZE))) { cancel(); return false; }
  {
    std::lock_guard lock(mutex);
    if (pending.size() >= 128) { ++epoch; pending.clear(); }
    pending.push_back({root, info.hwndFocus, process, static_cast<wchar_t>(key.vkCode), GetTickCount64(), epoch.load(), command});
    candidatePending = true;
  }
  wake.notify_one();
  if (choosing && key.vkCode < 256) captured[key.vkCode] = true;
  return choosing;
}
void mouse(WPARAM message, const MSLLHOOKSTRUCT &event) {
  if (message != WM_LBUTTONDOWN && message != WM_RBUTTONDOWN && message != WM_MBUTTONDOWN) return;
  const auto popup = candidateWindow.load();
  const auto under = WindowFromPoint(event.pt);
  if (!popup || (under != popup && !IsChild(popup, under))) cancel();
}
}
