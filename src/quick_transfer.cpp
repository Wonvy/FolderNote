#include "quick_transfer.h"
#include <shlobj.h>
#include <shellapi.h>
using Microsoft::WRL::ComPtr;

namespace quick::transfer {
namespace {
ComPtr<IShellItemArray> snapshot(IShellItemArray *selection, MenuMode mode) {
  DWORD count = 0;
  if (!selection || FAILED(selection->GetCount(&count)) || !count) return {};
  struct Ids {
    std::vector<PIDLIST_ABSOLUTE> values;
    ~Ids() { for (auto id : values) CoTaskMemFree(id); }
  } ids;
  ids.values.resize(count);
  const SFGAOF required = SFGAO_FILESYSTEM | (mode == MenuMode::Move ? SFGAO_CANMOVE : mode == MenuMode::Copy ? SFGAO_CANCOPY : 0);
  for (DWORD index = 0; index < count; ++index) {
    ComPtr<IShellItem> item; SFGAOF attributes = 0;
    if (FAILED(selection->GetItemAt(index, &item)) || !item ||
        FAILED(item->GetAttributes(required, &attributes)) || (attributes & required) != required ||
        FAILED(SHGetIDListFromObject(item.Get(), &ids.values[index]))) return {};
  }
  ComPtr<IShellItemArray> result;
  SHCreateShellItemArrayFromIDLists(count, const_cast<PCIDLIST_ABSOLUTE *>(ids.values.data()), &result);
  return result;
}
// Kept separate so the contract with the Windows copy engine can be tested
// without displaying system dialogs or touching any user files.
HRESULT execute(IFileOperation *operation, HWND owner, IShellItemArray *items, IShellItem *destination,
                MenuMode mode, const std::function<bool()> &current) {
  if (!operation || !items || !destination || (mode != MenuMode::Copy && mode != MenuMode::Move)) return E_INVALIDARG;
  DWORD count = 0; SFGAOF attributes = 0;
  HRESULT hr = items->GetCount(&count);
  if (FAILED(hr)) return hr;
  if (!count) return E_INVALIDARG;
  hr = destination->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes);
  if (FAILED(hr)) return hr;
  if ((attributes & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM)) return E_INVALIDARG;
  // Retain standard progress, collision/error dialogs and Explorer undo.
  // Do not silently overwrite, auto-rename or suppress the system UI.
  hr = operation->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD);
  if (SUCCEEDED(hr)) hr = operation->SetOwnerWindow(owner);
  if (SUCCEEDED(hr) && !current()) return S_FALSE;
  if (SUCCEEDED(hr)) hr = mode == MenuMode::Copy ? operation->CopyItems(items, destination) : operation->MoveItems(items, destination);
  if (FAILED(hr)) return hr;
  if (!current()) return S_FALSE;
  hr = operation->PerformOperations();
  BOOL aborted = FALSE;
  const auto status = operation->GetAnyOperationsAborted(&aborted);
  if (SUCCEEDED(status) && aborted) return S_FALSE;
  return FAILED(hr) ? hr : status;
}
}
ComPtr<IShellItemArray> selectedItems(IShellView *shellView, DWORD &count) {
  count = 0;
  if (!shellView) return {};
  ComPtr<IFolderView2> folder; ComPtr<IShellItemArray> selection;
  DWORD selectedCount = 0;
  if (FAILED(shellView->QueryInterface(IID_PPV_ARGS(&folder))) || !folder ||
      FAILED(folder->GetSelection(FALSE, &selection)) || !selection ||
      FAILED(selection->GetCount(&selectedCount))) return {};
  count = selectedCount;
  // Capture the view's selection regardless of the pointer location. Keep its
  // count even when unsupported items prevent a transferable filesystem snapshot.
  return snapshot(selection.Get(), MenuMode::Navigate);
}
bool supports(IShellItemArray *items, MenuMode mode) {
  if (!items || (mode != MenuMode::Copy && mode != MenuMode::Move)) return false;
  DWORD count = 0; SFGAOF attributes = 0;
  const SFGAOF required = SFGAO_FILESYSTEM | (mode == MenuMode::Move ? SFGAO_CANMOVE : SFGAO_CANCOPY);
  return SUCCEEDED(items->GetCount(&count)) && count &&
      SUCCEEDED(items->GetAttributes(SIATTRIBFLAGS_AND, required, &attributes)) && (attributes & required) == required;
}
HRESULT perform(HWND owner, IShellItemArray *items, IShellItem *destination, MenuMode mode, const std::function<bool()> &current) {
  ComPtr<IFileOperation> operation;
  const auto hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
  return FAILED(hr) ? hr : execute(operation.Get(), owner, items, destination, mode, current);
}
}
