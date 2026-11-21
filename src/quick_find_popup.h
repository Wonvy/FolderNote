#pragma once
#include "quick_find.h"
#include "theme.h"
#include "quick_icons.h"
#include <functional>
#include <windows.h>

namespace quick_find {
struct CandidateIcon {
  quick::icons::Source source = quick::icons::Source::system(SIID_DOCNOASSOC);
  quick::icons::Image image;
  CandidateIcon() = default;
  CandidateIcon(quick::icons::Source value) : source(std::move(value)) {}
  static CandidateIcon copy(HICON value) {
    CandidateIcon result;
    if (auto icon = value ? CopyIcon(value) : nullptr) {
      try { result.image = std::make_shared<quick::icons::Icon>(icon); }
      catch (...) { DestroyIcon(icon); throw; }
    }
    return result;
  }
};
// A modeless, non-activating candidate list. All methods run on its UI thread.
// The caller keeps keyboard focus and only acts on an item after confirmation.
class Popup {
public:
  using Choose = std::function<void(int)>;
  Popup() = default;
  ~Popup();
  Popup(const Popup &) = delete;
  Popup &operator=(const Popup &) = delete;
  void begin(HWND anchor, const std::vector<std::wstring> &labels, Choose choose, std::vector<CandidateIcon> icons = {});
  void close();
  bool active() const { return opened; }
  bool contains(HWND value) const;
  HWND handle() const { return window; }
  const std::wstring &query() const { return buffer; }
  void character(wchar_t value);
  bool key(UINT value);
private:
  HWND window = nullptr, list = nullptr, caption = nullptr, hint = nullptr;
  HWND context = nullptr, focus = nullptr;
  HHOOK mouse = nullptr;
  HFONT font = nullptr;
  ui::Theme theme;
  UINT dpi = 96;
  RECT anchorRect{};
  bool opened = false, show = false;
  std::wstring buffer;
  std::vector<std::wstring> labels;
  std::vector<Name> names;
  std::vector<int> rows;
  std::vector<CandidateIcon> icons;
  std::unique_ptr<quick::icons::Cache> iconCache;
  Choose choose;
  void refresh();
  void confirm();
  int px(int value) const { return MulDiv(value, dpi, 96); }
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK listProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  static LRESULT CALLBACK outsideMouse(int, WPARAM, LPARAM);
};
}
