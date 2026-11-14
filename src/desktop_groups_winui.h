#pragma once
#include "desktop_groups_model.h"
#include <windows.h>
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

struct Events {
  std::function<void(std::vector<int>)> selection;
  std::function<void(int)> open;
  std::function<void(POINT)> context;
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
  Surface(HWND parent, Events events);
  ~Surface();
  Surface(const Surface &) = delete;
  Surface &operator=(const Surface &) = delete;
  bool available() const;
  void place(RECT bounds);
  void show(bool visible);
  void animateReflow(bool resizing);
  void animateZoom(bool zoomIn);
  bool onFrame(std::function<void()> callback);
  void rows(const std::vector<Item> &items, bool listMode, int logicalIconSize, int spacing = 0);
  void itemSpacing(int spacing);
  int pageCapacity(bool listMode, int logicalIconSize, int titleHeight, int cornerRadius, int spacing, bool showDots) const;
  void pagination(int page, int pages, std::function<void(int)> navigate);
  void appearance(bool dark, bool highContrast, COLORREF tint, BYTE opacity, bool glass);
  bool acrylicActive() const;
  void header(const std::vector<std::wstring> &titles, std::size_t active, bool collapsed,
              bool actionsVisible, bool highlighted, int radius,
              COLORREF text, COLORREF muted, COLORREF border, COLORREF accent,
              int height, TitleStyle style, HICON icon);
  void showActions(bool visible);
  void showCollapseMove(bool visible);
  bool beginTitleRename(std::function<void(std::wstring)> commit);
  void highlight(bool highlighted);
  bool headerInteractive(POINT screen) const;
  std::optional<std::size_t> tabAt(POINT screen) const;
  std::optional<std::size_t> tabInsertion(POINT screen) const;
  void showTabInsertion(std::optional<std::size_t> slot);
  bool wheelOnTab(POINT screen, int delta);
  int hit(POINT screen) const;
  bool itemBounds(int row, RECT &screen) const;
  bool imageBounds(int row, RECT &screen) const;
  bool scrollAt(POINT screen);
  void refreshImage(int row, HICON image);
  void select(const std::vector<int> &rows);

private:
  struct State;
  std::unique_ptr<State> state;
};

}
