// Manual fixture and --verify integration check. Neither writes a file.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include "../src/quick_dialog.h"
#include <atomic>
#include <memory>
#include <string>
#include <sstream>
using Microsoft::WRL::ComPtr;
namespace {
constexpr wchar_t filename[] = L"保留这个文件名.txt";
void log(const std::wstring &line) {
  auto text = line + L"\r\n";
  int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
  std::string bytes(size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), bytes.data(), size, nullptr, nullptr);
  DWORD written = 0; auto output = GetStdHandle(STD_OUTPUT_HANDLE);
  if (output && output != INVALID_HANDLE_VALUE) WriteFile(output, bytes.data(), DWORD(bytes.size()), &written, nullptr);
  else OutputDebugStringW(text.c_str());
}
std::wstring hex(HRESULT value) { std::wostringstream text; text << L"0x" << std::hex << static_cast<unsigned long>(value); return text.str(); }
const wchar_t *name(quick::dialog::NavigateResult value) {
  using quick::dialog::NavigateResult;
  switch (value) {
  case NavigateResult::Success: return L"Success";
  case NavigateResult::TargetChanged: return L"TargetChanged";
  case NavigateResult::ActivationFailed: return L"ActivationFailed";
  case NavigateResult::ModifierPressed: return L"ModifierPressed";
  case NavigateResult::InputBlocked: return L"InputBlocked";
  case NavigateResult::AddressUnavailable: return L"AddressUnavailable";
  case NavigateResult::AddressWriteFailed: return L"AddressWriteFailed";
  case NavigateResult::AddressSubmitFailed: return L"AddressSubmitFailed";
  }
  return L"Unknown";
}
bool owned(HWND window) { DWORD pid = 0; return IsWindow(window) && GetWindowThreadProcessId(window, &pid) && pid == GetCurrentProcessId(); }
DWORD WINAPI automationTree(void *argument) {
  const auto root = reinterpret_cast<HWND>(argument);
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return 0;
  try {
    ComPtr<IUIAutomation> automation; ComPtr<IUIAutomationElement> element;
    auto result = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    if (SUCCEEDED(result) && owned(root)) result = automation->ElementFromHandle(root, &element);
    if (FAILED(result) || !element) log(L"uia-address-root: unavailable " + hex(result));
    else {
      ComPtr<IUIAutomationCondition> condition; ComPtr<IUIAutomationElementArray> elements;
      result = automation->CreateTrueCondition(&condition);
      if (SUCCEEDED(result)) result = element->FindAll(TreeScope_Subtree, condition.Get(), &elements);
      int count = 0; if (elements) elements->get_Length(&count);
      log(L"uia-address-root: result=" + hex(result) + L" elements=" + std::to_wstring(count));
      for (int index = 0; index < count && index < 64 && owned(root); ++index) {
        ComPtr<IUIAutomationElement> current;
        if (FAILED(elements->GetElement(index, &current)) || !current) continue;
        CONTROLTYPEID type = 0; BOOL offscreen = FALSE; int pid = 0;
        current->get_CurrentProcessId(&pid);
        if (pid != static_cast<int>(GetCurrentProcessId())) continue;
        current->get_CurrentControlType(&type); current->get_CurrentIsOffscreen(&offscreen);
        BSTR cls = nullptr, id = nullptr, label = nullptr;
        current->get_CurrentClassName(&cls); current->get_CurrentAutomationId(&id); current->get_CurrentName(&label);
        ComPtr<IUnknown> invoke, value, legacy;
        current->GetCurrentPattern(UIA_InvokePatternId, &invoke);
        current->GetCurrentPattern(UIA_ValuePatternId, &value);
        current->GetCurrentPattern(UIA_LegacyIAccessiblePatternId, &legacy);
        log(L"uia[" + std::to_wstring(index) + L"] type=" + std::to_wstring(type) +
            L" class=" + (cls ? cls : L"") + L" automationId=" + (id ? id : L"") +
            L" name=" + (label ? label : L"") + L" offscreen=" + std::to_wstring(offscreen != FALSE) +
            L" Invoke=" + std::to_wstring(invoke != nullptr) + L" Value=" + std::to_wstring(value != nullptr) +
            L" LegacyIAccessible=" + std::to_wstring(legacy != nullptr));
        SysFreeString(cls); SysFreeString(id); SysFreeString(label);
      }
    }
  } catch (...) { log(L"uia-address-root: diagnostic exception"); }
  CoUninitialize(); return 0;
}
void dumpAutomation(HWND root) {
  if (!owned(root)) return;
  HANDLE thread = CreateThread(nullptr, 0, automationTree, root, 0, nullptr);
  if (!thread) { log(L"uia-address-root: diagnostic worker unavailable"); return; }
  const auto deadline = GetTickCount64() + 2500;
  while (WaitForSingleObject(thread, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
    MsgWaitForMultipleObjects(1, &thread, FALSE, 25, QS_SENDMESSAGE);
    // Service synchronous provider requests without reentering the probe timer.
    MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
  }
  if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) log(L"uia-address-root: diagnostic deadline reached");
  CloseHandle(thread);
}
void focusTree(HWND window) {
  if (!owned(window)) { log(L"focus: fixture no longer available"); return; }
  GUITHREADINFO info{sizeof(info)}; GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info);
  log(L"focus: fixture-foreground=" + std::to_wstring(GetForegroundWindow() == window));
  wchar_t foregroundClass[128]{}; DWORD foregroundProcess = 0;
  auto foreground = GetForegroundWindow();
  GetClassNameW(foreground, foregroundClass, ARRAYSIZE(foregroundClass));
  GetWindowThreadProcessId(foreground, &foregroundProcess);
  log(std::wstring(L"foreground class=") + foregroundClass + L" own-process=" + std::to_wstring(foregroundProcess == GetCurrentProcessId()));
  auto print = [](HWND control, HWND focus) {
    wchar_t cls[128]{}, parentClass[128]{}; GetClassNameW(control, cls, ARRAYSIZE(cls));
    const auto parent = GetParent(control); GetClassNameW(parent, parentClass, ARRAYSIZE(parentClass));
    log(std::wstring(L"control class=") + cls + L" id=" + std::to_wstring(GetDlgCtrlID(control)) +
        L" visible=" + std::to_wstring(IsWindowVisible(control) != FALSE) + L" focused=" + std::to_wstring(control == focus) +
        L" parent=" + parentClass + L" parentId=" + std::to_wstring(GetDlgCtrlID(parent)));
    if (!wcscmp(cls, TOOLBARCLASSNAMEW)) {
      const auto count = SendMessageW(control, TB_BUTTONCOUNT, 0, 0);
      log(L"toolbar: buttons=" + std::to_wstring(count));
      for (int index = 0; index < count && index < 24; ++index) {
        TBBUTTON button{};
        if (SendMessageW(control, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&button)))
          log(L"toolbar-button: index=" + std::to_wstring(index) + L" command=" + std::to_wstring(button.idCommand) +
              L" state=" + std::to_wstring(button.fsState) + L" style=" + std::to_wstring(button.fsStyle));
      }
    }
  };
  for (HWND current = info.hwndFocus; current && (current == window || IsChild(window, current)); current = GetParent(current)) {
    print(current, info.hwndFocus); if (current == window) break;
  }
  struct Context { decltype(print) *output; HWND focus; unsigned count = 0; HWND address = nullptr; } context{&print, info.hwndFocus};
  EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL {
    auto &state = *reinterpret_cast<Context *>(value); wchar_t cls[128]{}; GetClassNameW(child, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Address Band Root")) state.address = child;
    if (!wcscmp(cls, L"Edit") || wcsstr(cls, L"Breadcrumb") || wcsstr(cls, L"Address") || wcsstr(cls, L"ComboBox") ||
        !wcscmp(cls, TOOLBARCLASSNAMEW)) {
      (*state.output)(child, state.focus); if (++state.count == 64) return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&context));
  if (context.address) dumpAutomation(context.address);
}
bool samePath(std::wstring a, std::wstring b) {
  while (a.size() > 3 && a.back() == L'\\') a.pop_back();
  while (b.size() > 3 && b.back() == L'\\') b.pop_back();
  return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}
struct Worker {
  HWND target = nullptr;
  std::wstring destination, requestName;
  std::atomic<bool> cancelled{false}, done{false};
  std::atomic<quick::dialog::NavigateResult> result{quick::dialog::NavigateResult::TargetChanged};
};
DWORD WINAPI work(void *argument) {
  std::unique_ptr<std::shared_ptr<Worker>> argumentOwner(static_cast<std::shared_ptr<Worker> *>(argument));
  auto state = *argumentOwner; const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  try {
    quick::detail::MenuRequest request(state->requestName.c_str()); request.begin();
    if (!state->cancelled && request.valid() && owned(state->target) && GetForegroundWindow() == state->target)
      state->result = quick::dialog::navigate(state->target, state->destination, request);
  } catch (...) { state->result = quick::dialog::NavigateResult::TargetChanged; }
  if (SUCCEEDED(initialized)) CoUninitialize(); state->done = true; return 0;
}
struct Verification {
  ComPtr<IFileSaveDialog> dialog;
  std::shared_ptr<Worker> worker = std::make_shared<Worker>();
  HWND target = nullptr, owner = nullptr; HANDLE thread = nullptr;
  std::wstring initial, destination;
  ULONGLONG started = GetTickCount64(), navigationStarted = 0;
  bool initialReady = false, reached = false, preserved = false, finished = false, passed = false, reported = false;
  bool activationAttempted = false;
  void cancelWorker() {
    worker->cancelled = true;
    if (!worker->requestName.empty()) { quick::detail::MenuRequest cancel(worker->requestName.c_str()); cancel.begin(); }
  }
  void close(bool success) {
    if (finished) return; finished = true; passed = success; cancelWorker(); dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
  }
  void folder(IFileDialog *current) {
    ComPtr<IShellItem> item; PWSTR path = nullptr;
    if (FAILED(current->GetFolder(&item)) || !item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    const std::wstring actual(path); CoTaskMemFree(path);
    if (samePath(actual, initial)) initialReady = true;
    if (!samePath(actual, destination)) return;
    reached = true; PWSTR value = nullptr; const auto result = current->GetFileName(&value);
    preserved = SUCCEEDED(result) && value && !wcscmp(value, filename); CoTaskMemFree(value);
    // Only fixture outcome labels are logged, never user paths or filenames.
    log(L"folder-event: actual=Windows-destination filename-preserved=" + std::to_wstring(preserved) + L" read=" + hex(result));
  }
  void tick() {
    if (finished) return;
    if (!target) { ComPtr<IOleWindow> window; if (SUCCEEDED(dialog.As(&window))) window->GetWindow(&target); }
    if ((navigationStarted && GetTickCount64() - navigationStarted >= 12000) ||
        (!navigationStarted && GetTickCount64() - started >= 60000)) {
      log(L"FAIL timeout: destination-folder-event=" + std::to_wstring(reached)); focusTree(target); close(false); return;
    }
    if (worker->done && !reported) {
      reported = true; log(std::wstring(L"navigate-result: ") + name(worker->result.load()) + L" folder-event=" + std::to_wstring(reached));
      if (worker->result != quick::dialog::NavigateResult::Success) { focusTree(target); close(false); return; }
    }
    if (worker->done && reached) { close(worker->result == quick::dialog::NavigateResult::Success && preserved); return; }
    if (thread || !initialReady || !owned(target) || !IsWindowVisible(target)) return;
    // A hidden process launch can leave the owned dialog inactive. Request
    // activation once during setup; never keep taking focus from the user.
    if (!activationAttempted) {
      activationAttempted = true;
      const BOOL activated = GetForegroundWindow() == target || SetForegroundWindow(target);
      log(L"fixture-activation: requested-once=" + std::to_wstring(activated != FALSE));
    }
    if (GetForegroundWindow() != target) return;
    worker->target = target; worker->destination = destination;
    navigationStarted = GetTickCount64();
    worker->requestName = L"Local\\FolderNote.DialogProbe." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(started);
    auto argument = new std::shared_ptr<Worker>(worker);
    log(L"navigate-start: owned foreground Save dialog; fixture temporary folder -> Windows folder");
    thread = CreateThread(nullptr, 0, work, argument, 0, nullptr);
    if (!thread) { delete argument; log(L"FAIL worker creation"); close(false); }
  }
};
class Events final : public IFileDialogEvents {
  ULONG refs = 1; Verification *state;
public:
  explicit Events(Verification *value) : state(value) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IFileDialogEvents) return E_NOINTERFACE;
    *out = static_cast<IFileDialogEvents *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override { auto count = --refs; if (!count) delete this; return count; }
  HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *) override { log(L"save-blocked: verification never accepts a file"); return S_FALSE; }
  HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *dialog) override { state->folder(dialog); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog *, IShellItem *, FDE_SHAREVIOLATION_RESPONSE *response) override { if (response) *response = FDESVR_REFUSE; return S_OK; }
  HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog *, IShellItem *, FDE_OVERWRITE_RESPONSE *response) override { if (response) *response = FDEOR_REFUSE; return S_OK; }
};
LRESULT CALLBACK ownerProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams));
  auto state = reinterpret_cast<Verification *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (state && message == WM_TIMER && w == 71) { state->tick(); return 0; }
  return DefWindowProcW(window, message, w, l);
}
int verify() {
  Verification state;
  auto created = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&state.dialog));
  if (FAILED(created)) { log(L"FAIL create Save dialog " + hex(created)); return 1; }
  wchar_t temp[32768]{}, system[MAX_PATH]{}; auto size = GetTempPathW(ARRAYSIZE(temp), temp);
  if (!size || size >= ARRAYSIZE(temp) || !GetWindowsDirectoryW(system, ARRAYSIZE(system))) { log(L"FAIL fixture folders"); return 1; }
  state.initial = temp; state.destination = system;
  if (samePath(state.initial, state.destination)) { log(L"FAIL identical fixture folders"); return 1; }
  ComPtr<IShellItem> initial;
  if (FAILED(SHCreateItemFromParsingName(state.initial.c_str(), nullptr, IID_PPV_ARGS(&initial))) || FAILED(state.dialog->SetFolder(initial.Get()))) return 1;
  DWORD options = 0; state.dialog->GetOptions(&options); state.dialog->SetOptions(options | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR);
  state.dialog->SetTitle(L"FolderNote 自动定位验证（自动取消，不保存文件）"); state.dialog->SetFileName(filename);
  WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = ownerProc; cls.lpszClassName = L"FolderNote.DialogProbe.Owner.v1";
  if (!RegisterClassW(&cls)) return 1;
  state.owner = CreateWindowExW(WS_EX_TOOLWINDOW, cls.lpszClassName, L"Probe owner", WS_OVERLAPPED,
      0, 0, 0, 0, nullptr, nullptr, cls.hInstance, &state);
  ComPtr<Events> events; events.Attach(new Events(&state)); DWORD cookie = 0;
  auto advised = state.dialog->Advise(events.Get(), &cookie); HRESULT shown = E_FAIL;
  if (state.owner && SUCCEEDED(advised) && SetTimer(state.owner, 71, 50, nullptr)) {
    log(L"verify: opening owned native Save dialog; filename submission is blocked"); shown = state.dialog->Show(state.owner);
  } else log(L"FAIL observer/timer setup");
  state.cancelWorker(); if (state.owner) KillTimer(state.owner, 71);
  if (state.thread) {
    auto deadline = GetTickCount64() + 4000;
    while (WaitForSingleObject(state.thread, 0) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
      MsgWaitForMultipleObjects(1, &state.thread, FALSE, 50, QS_ALLINPUT);
      MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (WaitForSingleObject(state.thread, 0) != WAIT_OBJECT_0) { state.passed = false; log(L"FAIL bounded worker cleanup"); }
    CloseHandle(state.thread);
  }
  if (SUCCEEDED(advised)) state.dialog->Unadvise(cookie);
  if (state.owner) DestroyWindow(state.owner); UnregisterClassW(cls.lpszClassName, cls.hInstance);
  log(std::wstring(state.passed ? L"PASS" : L"FAIL") + L" native Save navigation: result=" + name(state.worker->result.load()) +
      L" destination-folder-event=" + std::to_wstring(state.reached) + L" filename-preserved=" + std::to_wstring(state.preserved) + L" dialog-close=" + hex(shown));
  return state.passed ? 0 : 1;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  int count = 0; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  const bool automated = count == 2 && !wcscmp(arguments[1], L"--verify"); LocalFree(arguments);
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
  int result = 0;
  if (automated) result = verify();
  else {
    ComPtr<IFileSaveDialog> dialog;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      DWORD options = 0; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_DONTADDTORECENT);
      dialog->SetTitle(L"FolderNote 快速定位测试（不会写入文件）");
      dialog->SetFileName(filename);
      dialog->Show(nullptr);
    }
  }
  CoUninitialize(); return result;
}
