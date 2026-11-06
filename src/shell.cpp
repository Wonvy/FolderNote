#include "core.h"
#include "navigation.h"
#include "resource.h"
#include <atomic>
#include <shobjidl.h>
#include <shlwapi.h>
static HMODULE module;
static std::atomic<long> objects{0};
static constexpr int settingsOperation = 3;
static const wchar_t *commandId(int operation) {
  return operation == settingsOperation ? fn::settingsId : fn::ids[operation];
}
static HRESULT launch(const std::wstring &arguments) {
  auto exe = fn::modulePath(module).parent_path() / L"FolderNote.exe";
  std::wstring command = L"\"" + exe.native() + L"\" " + arguments;
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, nullptr, &startup, &process))
    return HRESULT_FROM_WIN32(GetLastError());
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return S_OK;
}
class Command : public IExecuteCommand, public IObjectWithSelection, public IExplorerCommand {
  std::atomic<ULONG> refs{1};
  int op;
  IShellItemArray *selection = nullptr;

public:
  Command(int o) : op(o) { ++objects; }
  ~Command() {
    if (selection)
      selection->Release();
    --objects;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    if (id == IID_IUnknown || id == IID_IExecuteCommand)
      *p = static_cast<IExecuteCommand *>(this);
    else if (id == IID_IObjectWithSelection)
      *p = static_cast<IObjectWithSelection *>(this);
    else if (id == IID_IExplorerCommand)
      *p = static_cast<IExplorerCommand *>(this);
    else
      return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  HRESULT STDMETHODCALLTYPE GetTitle(IShellItemArray*, PWSTR* p) override {
    if (!p) return E_POINTER;
    return SHStrDupW(op == settingsOperation ? L"FolderNote 设置" : fn::titles[op], p);
  }
  HRESULT STDMETHODCALLTYPE GetIcon(IShellItemArray *, PWSTR *p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    try {
      const auto executable = fn::modulePath(module).parent_path() / L"FolderNote.exe";
      const auto icon = L"\"" + executable.native() + L"\",-" + std::to_wstring(IDI_FOLDERNOTE);
      return SHStrDupW(icon.c_str(), p);
    } catch (...) {
      return E_FAIL;
    }
  }
  HRESULT STDMETHODCALLTYPE GetToolTip(IShellItemArray*, PWSTR* p) override { *p=nullptr; return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalName(GUID* p) override {
    if (!p) return E_POINTER;
    return CLSIDFromString(commandId(op), p);
  }
  HRESULT STDMETHODCALLTYPE GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
    if(!state)return E_POINTER;
    DWORD count=0; *state=ECS_HIDDEN;
    // Compatibility for a cached 1.4.0 menu: Settings now lives in Start.
    if (op == settingsOperation) return S_OK;
    if (!navigation::notesEnabled()) return S_OK;
    if(items&&SUCCEEDED(items->GetCount(&count))&&count&&(op!=0||count==1))*state=ECS_ENABLED;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Invoke(IShellItemArray* items, IBindCtx*) override { SetSelection(items); return Execute(); }
  HRESULT STDMETHODCALLTYPE GetFlags(EXPCMDFLAGS* p) override { *p=ECF_DEFAULT;return S_OK; }
  HRESULT STDMETHODCALLTYPE EnumSubCommands(IEnumExplorerCommand** p) override { *p=nullptr;return E_NOTIMPL; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE SetSelection(IShellItemArray *p) override {
    if (selection)
      selection->Release();
    selection = p;
    if (p)
      p->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSelection(REFIID id, void **p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    return selection ? selection->QueryInterface(id, p) : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE SetKeyState(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetParameters(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetPosition(POINT) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetShowWindow(int) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetNoShowUI(BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetDirectory(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Execute() override {
    try {
      if (op == settingsOperation)
        return launch(L"--settings");
      // A menu can remain open while settings change. Recheck before creating
      // a request or launching a note operation from that stale menu.
      if (!navigation::notesEnabled())
        return E_ACCESSDENIED;
      if (!selection)
        return E_FAIL;
      DWORD count = 0;
      HRESULT hr = selection->GetCount(&count);
      if (FAILED(hr))
        return hr;
      if (!count || (op == 0 && count != 1))
        return E_INVALIDARG;
      std::vector<std::wstring> paths;
      for (DWORD i = 0; i < count; ++i) {
        IShellItem *item = nullptr;
        hr = selection->GetItemAt(i, &item);
        if (FAILED(hr))
          return hr;
        PWSTR path = nullptr;
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        item->Release();
        if (FAILED(hr))
          return hr;
        paths.emplace_back(path);
        CoTaskMemFree(path);
      }
      auto token = fn::makeRequest(op, paths);
      return launch(L"--request " + token);
    } catch (...) {
      return E_FAIL;
    }
  }
};
class Factory : public IClassFactory {
  std::atomic<ULONG> refs{1};
  int op;

public:
  Factory(int o) : op(o) { ++objects; }
  ~Factory() { --objects; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
    if (!p)
      return E_POINTER;
    *p = nullptr;
    if (id != IID_IUnknown && id != IID_IClassFactory)
      return E_NOINTERFACE;
    *p = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID id,
                                           void **p) override {
    if (outer)
      return CLASS_E_NOAGGREGATION;
    auto c = new (std::nothrow) Command(op);
    if (!c)
      return E_OUTOFMEMORY;
    auto hr = c->QueryInterface(id, p);
    c->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL b) override {
    objects += b ? 1 : -1;
    return S_OK;
  }
};
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID cls, REFIID id,
                                               void **p) {
  if (!p)
    return E_POINTER;
  *p = nullptr;
  for (int i = 0; i <= settingsOperation; ++i) {
    GUID g;
    CLSIDFromString(commandId(i), &g);
    if (cls == g) {
      auto f = new (std::nothrow) Factory(i);
      if (!f)
        return E_OUTOFMEMORY;
      auto hr = f->QueryInterface(id, p);
      f->Release();
      return hr;
    }
  }
  return CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
  return objects == 0 ? S_OK : S_FALSE;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    module = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}
