#pragma once
#include "desktop_groups_model.h"
#include <windows.h>
#include <oleidl.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace desktop_groups::winui {
inline constexpr LRESULT titleHeightHit = 0x100;

struct Item {
  std::wstring name;
  std::wstring path;
  HICON image = nullptr;
};

struct TransferVisual {
  double progress = 0.0; // < 0: lifted drag ghost only; 0..1: macOS pie-ring progress
  bool outgoing = false;
};

struct Events {
  std::function<void(std::vector<int>)> selection;
  std::function<void(int)> open;
  std::function<void(int)> rename;
  std::function<bool(UINT, bool, bool, bool)> shortcut;
  std::function<bool(int)> suppressToolTip;
  std::function<bool(int)> preview;
  std::function<bool()> previewAvailable;
  std::function<void()> previewFocus;
  std::function<bool(UINT, WPARAM)> findInput;
  std::function<void(POINT)> context;
  std::function<void(POINT)> middleClick;
  std::function<void(int)> zoom;
  std::function<void(int)> beginDrag;
  std::function<void(std::size_t)> tab;
  std::function<bool(int)> tabWheel;
  std::function<void(std::size_t, POINT, RECT)> tabPressed;
  std::function<void(POINT)> tabMoved;
  std::function<void(POINT)> tabReleased;
  std::function<void()> tabCanceled;
  std::function<void(POINT)> collapsePressed;
  std::function<void(POINT)> collapseMoved;
  std::function<bool()> collapseReleased;
  std::function<void()> collapseCanceled;
  std::function<void(int)> headerCommand;
  std::function<void(POINT)> headerDrag;
  std::function<bool(int)> wheel;
};

class Runtime {
public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  bool available() const;
  bool preTranslate(MSG &message) const;

private:
  struct State;
  std::unique_ptr<State> state;
};

class Surface {
public:
  Surface(HWND parent, Events events, IDropTarget *dropTarget = nullptr);
  ~Surface();
  Surface(const Surface &) = delete;
  Surface &operator=(const Surface &) = delete;
  bool available() const;
  void place(RECT bounds);
  void focus(int row = -1);
  // nullopt outside the file list, -1 for a focused list with no selection.
  std::optional<int> previewRow() const;
  void show(bool visible);
  void animateReflow(bool resizing);
  void animateZoom(bool zoomIn);
  bool onFrame(std::function<void()> callback);
  bool hasCachedImage(const std::wstring &path, bool listMode, int logicalIconSize) const;
  void rows(const std::vector<Item> &items, bool listMode, int logicalIconSize, int spacing = 0);
  void itemSpacing(int spacing);
  int pageCapacity(bool listMode, int logicalIconSize, int titleHeight, int cornerRadius, int spacing, bool showDots) const;
  void pagination(int page, int pages, std::function<void(int)> navigate);
  void appearance(bool dark, bool highContrast, COLORREF tint, BYTE opacity, bool glass);
  bool acrylicActive() const;
  bool usesDesktopGlass() const;
  void header(const std::vector<std::wstring> &titles, std::size_t active, bool collapsed,
              bool actionsVisible, bool highlighted, int radius,
              COLORREF text, COLORREF muted, COLORREF border, COLORREF accent,
              int height, TitleStyle style, HICON icon);
  void closeToolTip();
  void showActions(bool visible);
  void showCollapseMove(bool visible);
  bool beginTitleRename(std::function<void(std::wstring)> commit);
  bool beginItemRename(int row, const std::wstring &initialText, int selectLength,
                       std::function<void(std::optional<std::wstring>)> commit);
  void cancelItemRename();
  void highlight(bool highlighted);
  bool headerInteractive(POINT screen) const;
  std::optional<std::size_t> tabAt(POINT screen) const;
  std::optional<std::size_t> tabInsertion(POINT screen) const;
  void showTabInsertion(std::optional<std::size_t> slot);
  void showItemInsertion(std::optional<RECT> screenBounds);
  void setDragActive(bool active);
  bool wheelOnTab(POINT screen, int delta);
  int hit(POINT screen) const;
  bool itemBounds(int row, RECT &screen) const;
  bool imageBounds(int row, RECT &screen) const;
  bool scrollAt(POINT screen);
  void refreshImage(int row, HICON image);
  void setTransfers(const std::vector<std::pair<std::wstring, TransferVisual>> &transfers);
  void select(const std::vector<int> &rows);
  void reveal(int row);

private:
  struct State;
  std::unique_ptr<State> state;
};

}
