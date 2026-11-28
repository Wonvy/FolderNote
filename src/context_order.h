#pragma once
#include <windows.h>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace context {
enum class PreviewTarget { Object, Background };
using MenuIcon = std::shared_ptr<std::remove_pointer_t<HICON>>;
struct MenuPreviewItem {
  std::wstring caption;
  MenuIcon icon;
  std::vector<MenuPreviewItem> children;
  bool separator = false;
  std::wstring handlerClsid;
};

// Enumerates the visible items and direct submenus in a classic Shell menu. An empty
// background path refers to the desktop namespace. Call from a COM STA.
// Only pass verified, ShibaKit-managed hidden handlers as hiddenHandlerCandidates.
// Their isolated preview items go into hiddenHandlerItems, outside the visible order.
std::vector<MenuPreviewItem> actualMenuOrder(HWND owner, const std::wstring &path,
                                             PreviewTarget target = PreviewTarget::Object,
                                             std::span<const std::wstring> handlerCandidates = {},
                                             std::span<const std::wstring> hiddenHandlerCandidates = {},
                                             std::vector<MenuPreviewItem> *hiddenHandlerItems = nullptr);
}
