#include "../src/quick_transfer.h"
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;

namespace {
int checks = 0;
void check(bool value, const char *reason) { ++checks; if (!value) throw std::runtime_error(reason); }
struct Unknown : IUnknown {
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **out) override { *out = nullptr; return E_NOINTERFACE; }
};
struct __declspec(uuid("b17b96ac-a474-4654-b0d4-8ad46a125d01")) SelectionView : Unknown {
  ComPtr<IShellItemArray> items;
  BOOL allowParent = TRUE;
  unsigned requests = 0;
  HRESULT result = S_OK;
  HRESULT GetSelection(BOOL fallback, IShellItemArray **out) {
    allowParent = fallback; ++requests; *out = nullptr;
    if (FAILED(result)) return result;
    if (!items) return S_FALSE;
    return items.CopyTo(out);
  }
};
struct SelectionSource : Unknown {
  SelectionView folder;
  bool supported = true;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (!supported || iid != __uuidof(SelectionView)) return E_NOINTERFACE;
    *out = &folder; folder.AddRef(); return S_OK;
  }
};
}
// Exercise production capture with an isolated Shell provider. The native item
// arrays below contain only disposable test files, never the user's selection.
#define IShellView SelectionSource
#define IFolderView2 SelectionView
#include "../src/quick_transfer.cpp"
#undef IFolderView2
#undef IShellView
namespace {
struct Operation final : IFileOperation {
  ComPtr<IFileOperation> real;
  DWORD flags = 0;
  int copies = 0, moves = 0, performed = 0;
  BOOL aborted = FALSE;
  HRESULT queueResult = S_OK, performResult = S_OK;
  IUnknown *received = nullptr;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    *out = nullptr;
    if (iid != IID_IUnknown && iid != __uuidof(IFileOperation)) return E_NOINTERFACE;
    *out = static_cast<IFileOperation *>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE Advise(IFileOperationProgressSink *, DWORD *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Unadvise(DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetOperationFlags(DWORD value) override {
    flags = value;
    // Native engine integration is restricted to disposable test files; do not
    // display test progress or replace the user's Explorer undo history.
    return real ? real->SetOperationFlags(FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE | FOF_NOCONFIRMMKDIR) : S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetProgressMessage(LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetProgressDialog(IOperationsProgressDialog *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetProperties(IPropertyChangeArray *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetOwnerWindow(HWND owner) override { return real ? real->SetOwnerWindow(owner) : S_OK; }
  HRESULT STDMETHODCALLTYPE ApplyPropertiesToItem(IShellItem *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE ApplyPropertiesToItems(IUnknown *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RenameItem(IShellItem *, LPCWSTR, IFileOperationProgressSink *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RenameItems(IUnknown *, LPCWSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE MoveItem(IShellItem *, IShellItem *, LPCWSTR, IFileOperationProgressSink *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE MoveItems(IUnknown *items, IShellItem *destination) override {
    ++moves; received = items; return real ? real->MoveItems(items, destination) : queueResult;
  }
  HRESULT STDMETHODCALLTYPE CopyItem(IShellItem *, IShellItem *, LPCWSTR, IFileOperationProgressSink *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE CopyItems(IUnknown *items, IShellItem *destination) override {
    ++copies; received = items; return real ? real->CopyItems(items, destination) : queueResult;
  }
  HRESULT STDMETHODCALLTYPE DeleteItem(IShellItem *, IFileOperationProgressSink *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DeleteItems(IUnknown *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE NewItem(IShellItem *, DWORD, LPCWSTR, LPCWSTR, IFileOperationProgressSink *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE PerformOperations() override { ++performed; return real ? real->PerformOperations() : performResult; }
  HRESULT STDMETHODCALLTYPE GetAnyOperationsAborted(BOOL *out) override {
    if (real) return real->GetAnyOperationsAborted(out);
    *out = aborted; return S_OK;
  }
};
ComPtr<IShellItem> itemAt(const std::filesystem::path &path) {
  ComPtr<IShellItem> item;
  check(SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))), "resolve test filesystem item"); return item;
}
struct Files {
  std::filesystem::path root;
  Files() {
    GUID id{}; CoCreateGuid(&id); wchar_t token[40]{}; StringFromGUID2(id, token, ARRAYSIZE(token));
    root = std::filesystem::temp_directory_path() / (std::wstring(L"FolderNote-TransferTests-") + token);
    std::filesystem::create_directories(root / L"source" / L"子目录 😀");
    std::filesystem::create_directories(root / L"copied"); std::filesystem::create_directories(root / L"moved");
    std::ofstream(root / L"source" / L"资料 with spaces.txt") << "test payload";
    std::ofstream(root / L"source" / L"子目录 😀" / L"child.txt") << "nested payload";
  }
  ~Files() { std::error_code error; std::filesystem::remove_all(root, error); }
};
void operationTests() {
  using quick::MenuMode; using quick::transfer::execute;
  SelectionSource source;
  DWORD count = 99;
  check(!quick::transfer::selectedItems(static_cast<SelectionSource *>(nullptr), count) && count == 0,
        "missing source clears displayed selection count");
  check(!quick::transfer::selectedItems(&source, count) && count == 0 && source.folder.requests == 1 && !source.folder.allowParent,
        "empty view selection never falls back to the containing folder");
  source.supported = false; count = 99;
  check(!quick::transfer::selectedItems(&source, count) && count == 0 && source.folder.requests == 1,
        "unsupported source clears count without querying another selection");
  source.supported = true; source.folder.result = E_ACCESSDENIED; count = 99;
  check(!quick::transfer::selectedItems(&source, count) && count == 0,
        "failed selection capture cannot expose a stale count");
  source.folder.result = S_OK;
  Files files;
  auto first = itemAt(files.root / L"source" / L"资料 with spaces.txt"), second = itemAt(files.root / L"source" / L"子目录 😀");
  PIDLIST_ABSOLUTE ids[2]{};
  check(SUCCEEDED(SHGetIDListFromObject(first.Get(), &ids[0])) && SUCCEEDED(SHGetIDListFromObject(second.Get(), &ids[1])), "get mixed file/folder identifiers");
  ComPtr<IShellItemArray> selection;
  const auto created = SHCreateShellItemArrayFromIDLists(2, const_cast<PCIDLIST_ABSOLUTE *>(ids), &selection);
  CoTaskMemFree(ids[0]); CoTaskMemFree(ids[1]); check(SUCCEEDED(created), "create a multi-item selection");
  source.folder.items = selection;
  auto snapshot = quick::transfer::selectedItems(&source, count);
  check(snapshot && count == 2 && !source.folder.allowParent,
        "view selection count and snapshot do not depend on mouse hit location");
  source.folder.items.Reset(); selection.Reset();
  check(SUCCEEDED(snapshot->GetCount(&count)) && count == 2, "snapshot remains independent of later view selection");
  check(!quick::transfer::selectedItems(&source, count) && count == 0,
        "clearing the active view selection clears the next menu count");
  check(quick::transfer::supports(snapshot.Get(), MenuMode::Copy) && quick::transfer::supports(snapshot.Get(), MenuMode::Move),
        "ordinary menu snapshot enables both transfer icons without separate settings");
  check(!quick::transfer::supports(nullptr, MenuMode::Copy) && !quick::transfer::supports(snapshot.Get(), MenuMode::Navigate),
        "missing selection and navigation are never transferable actions");
  check(!quick::transfer::snapshot(nullptr, MenuMode::Copy), "no selection never implies parent directory");
  ComPtr<IShellItem> computer; ComPtr<IShellItemArray> virtualSelection;
  check(SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_ComputerFolder, 0, nullptr, IID_PPV_ARGS(&computer))) &&
        SUCCEEDED(SHCreateShellItemArrayFromShellItem(computer.Get(), IID_PPV_ARGS(&virtualSelection))), "create virtual Shell selection");
  source.folder.items = virtualSelection;
  check(!quick::transfer::selectedItems(&source, count) && count == 1,
        "unsupported selected items retain their count while transfer stays unavailable");
  check(!quick::transfer::snapshot(virtualSelection.Get(), MenuMode::Copy) &&
        !quick::transfer::snapshot(virtualSelection.Get(), MenuMode::Move) &&
        !quick::transfer::snapshot(virtualSelection.Get(), MenuMode::Navigate) &&
        !quick::transfer::supports(virtualSelection.Get(), MenuMode::Copy) &&
        !quick::transfer::supports(virtualSelection.Get(), MenuMode::Move), "virtual namespace items cannot enable transfer icons");
  auto destination = itemAt(files.root / L"copied");
  for (auto mode : {MenuMode::Copy, MenuMode::Move}) {
    Operation operation;
    check(execute(&operation, nullptr, snapshot.Get(), destination.Get(), mode, [] { return true; }) == S_OK, "valid transfer dispatch succeeds");
    check(operation.copies == (mode == MenuMode::Copy ? 1 : 0) && operation.moves == (mode == MenuMode::Move ? 1 : 0) &&
          operation.performed == 1 && operation.received == snapshot.Get(), "all captured items are submitted once to the correct native operation");
    check((operation.flags & (FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_RENAMEONCOLLISION | FOFX_KEEPNEWERFILE)) == 0 &&
          (operation.flags & FOFX_ADDUNDORECORD), "production keeps native progress, conflicts, error handling and undo");
  }
  Operation cancelled;
  check(execute(&cancelled, nullptr, snapshot.Get(), destination.Get(), MenuMode::Move, [] { return false; }) == S_FALSE && !cancelled.moves && !cancelled.performed,
        "disabled or obsolete requests cannot queue moves");
  int validations = 0;
  check(execute(&cancelled, nullptr, snapshot.Get(), destination.Get(), MenuMode::Copy, [&] { return ++validations == 1; }) == S_FALSE && !cancelled.performed,
        "request invalidation during native setup prevents execution");
  Operation aborted; aborted.aborted = TRUE;
  check(execute(&aborted, nullptr, snapshot.Get(), destination.Get(), MenuMode::Copy, [] { return true; }) == S_FALSE, "native cancellation is not reported as success");
  Operation failed; failed.queueResult = E_ACCESSDENIED;
  check(execute(&failed, nullptr, snapshot.Get(), destination.Get(), MenuMode::Move, [] { return true; }) == E_ACCESSDENIED && !failed.performed,
        "queue failure cannot execute a partial operation");
  check(execute(&failed, nullptr, snapshot.Get(), first.Get(), MenuMode::Move, [] { return true; }) == E_INVALIDARG,
        "file destination is rejected before native execution");
  check(execute(&failed, nullptr, snapshot.Get(), destination.Get(), MenuMode::Navigate, [] { return true; }) == E_INVALIDARG,
        "ordinary navigation is not a transfer operation");
  Operation copying; check(SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&copying.real))), "create native copy engine");
  check(execute(&copying, nullptr, snapshot.Get(), destination.Get(), MenuMode::Copy, [] { return true; }) == S_OK, "native multi-item copy completes");
  for (const auto &base : {L"source", L"copied"}) {
    check(std::filesystem::exists(files.root / base / L"资料 with spaces.txt") &&
          std::filesystem::exists(files.root / base / L"子目录 😀" / L"child.txt"), "copy preserves sources and recursively copies folders with Unicode names");
  }
  Operation moving; check(SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&moving.real))), "create native move engine");
  destination = itemAt(files.root / L"moved");
  check(execute(&moving, nullptr, snapshot.Get(), destination.Get(), MenuMode::Move, [] { return true; }) == S_OK, "native multi-item move completes");
  check(!std::filesystem::exists(files.root / L"source" / L"资料 with spaces.txt") &&
        !std::filesystem::exists(files.root / L"source" / L"子目录 😀") &&
        std::filesystem::exists(files.root / L"moved" / L"资料 with spaces.txt") &&
        std::filesystem::exists(files.root / L"moved" / L"子目录 😀" / L"child.txt"), "move relocates both original files and folders");
  auto content = [&](const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  };
  for (const auto &base : {L"copied", L"moved"})
    check(content(files.root / base / L"子目录 😀" / L"child.txt") == "nested payload" &&
          content(files.root / base / L"资料 with spaces.txt") == "test payload", "copied and moved file contents remain byte-exact");
}
}
int main() {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 0;
  try { operationTests(); std::cout << "PASS " << checks << " transfer assertions: view selection, snapshots, native copy/move, progress flags, cancellation\n"; }
  catch (const std::exception &error) { std::cerr << "FAIL " << checks << ": " << error.what() << '\n'; result = 1; }
  CoUninitialize(); return result;
}
