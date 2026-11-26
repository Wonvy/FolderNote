#include "quick_dialog_history.h"
#include "quick_folders.h"
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>

namespace quick::dialog {
namespace {
std::wstring pathFromText(const std::wstring &text) {
  for (size_t i = 0; i + 2 < text.size(); ++i) {
    if ((text[i + 1] == L':' && text[i + 2] == L'\\') || (text[i] == L'\\' && text[i + 1] == L'\\')) {
      auto path = normalize(text.substr(i));
      if (!path.empty()) return path;
    }
  }
  return {};
}
}
std::wstring currentPath(HWND target) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IUIAutomation> automation;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) return {};
  ComPtr<IUIAutomation2> options;
  if (SUCCEEDED(automation.As(&options))) { options->put_ConnectionTimeout(200); options->put_TransactionTimeout(200); }
  ComPtr<IUIAutomationElement> root;
  ComPtr<IUIAutomationCondition> condition;
  VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_ToolBarControlTypeId;
  // UIA may return S_OK with a null out-pointer when the dialog closes or the
  // 200 ms timeout expires mid-query. This runs in the background helper, so a
  // null dereference here would kill middle-click for the whole session.
  if (FAILED(automation->ElementFromHandle(target, &root)) || !root ||
      FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)) || !condition) return {};
  ComPtr<IUIAutomationElementArray> bars;
  if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &bars)) || !bars) return {};
  int count = 0;
  if (FAILED(bars->get_Length(&count))) return {};
  for (int i = 0; i < std::min(count, 16); ++i) {
    ComPtr<IUIAutomationElement> element; BSTR name = nullptr;
    if (SUCCEEDED(bars->GetElement(i, &element)) && element && SUCCEEDED(element->get_CurrentName(&name))) {
      auto path = name ? pathFromText(name) : std::wstring(); SysFreeString(name);
      if (!path.empty()) return path;
    }
  }
  return {};
}
}
