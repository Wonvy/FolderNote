#pragma once
#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cwchar>
#include <utility>

namespace quick::detail {
// Presentation-only: a blank hit may show the current-folder caption. Selection
// capture and file operations must never depend on this optional UIA result.
inline bool blankSurface(HWND view, POINT point) {
  using Microsoft::WRL::ComPtr;
  auto withinView = [&] {
    HWND under = WindowFromPoint(point);
    return view && IsWindowVisible(view) && under && (under == view || IsChild(view, under));
  };
  if (!withinView()) return false;
  ComPtr<IUIAutomation> ui;
  ComPtr<IUIAutomation2> options;
  if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ui))) || !ui ||
      FAILED(ui.As(&options)) || !options || FAILED(options->put_ConnectionTimeout(150)) ||
      FAILED(options->put_TransactionTimeout(150))) return false;
  const auto started = GetTickCount64();
  auto expired = [&] { return GetTickCount64() - started >= 150; };
  ComPtr<IUIAutomationElement> root, element;
  ComPtr<IUIAutomationTreeWalker> walker;
  if (FAILED(ui->ElementFromHandle(view, &root)) || !root || expired() ||
      FAILED(ui->ElementFromPoint(point, &element)) || !element || expired() ||
      FAILED(ui->get_RawViewWalker(&walker)) || !walker) return false;
  bool contents = false;
  // Empty-folder text can be a passive child of ItemsView. File items, group
  // headers, editors, trees and buttons stop the walk even if an ancestor is
  // a list. The recognized list must also belong to this exact Shell view.
  for (unsigned depth = 0; element && depth < 16 && !expired(); ++depth) {
    BOOL equal = FALSE;
    if (FAILED(ui->CompareElements(root.Get(), element.Get(), &equal)) || expired()) return false;
    // Explorer's SHELLDLL_DefView may be a UIA Window (DUIListView), not
    // a passive pane. It is the verified boundary, not an intermediate control.
    if (equal && contents) return withinView();
    CONTROLTYPEID type{};
    if (FAILED(element->get_CurrentControlType(&type)) || expired()) return false;
    const bool surface = type == UIA_ListControlTypeId || type == UIA_DataGridControlTypeId || type == UIA_PaneControlTypeId;
    if (!surface && type != UIA_TextControlTypeId && type != UIA_CustomControlTypeId) return false;
    if (surface && !contents) {
      BSTR cls = nullptr, id = nullptr;
      const auto classResult = element->get_CurrentClassName(&cls);
      const auto idResult = expired() ? E_ABORT : element->get_CurrentAutomationId(&id);
      const bool recognized = (cls && (!wcscmp(cls, L"UIItemsView") || !wcscmp(cls, L"SysListView32"))) ||
          (id && !wcscmp(id, L"ItemsView"));
      SysFreeString(cls); SysFreeString(id);
      if (FAILED(classResult) || FAILED(idResult) || expired()) return false;
      contents = recognized;
    }
    if (equal) return contents && withinView();
    ComPtr<IUIAutomationElement> parent;
    if (FAILED(walker->GetParentElement(element.Get(), &parent))) return false;
    element = std::move(parent);
  }
  return false;
}
}
