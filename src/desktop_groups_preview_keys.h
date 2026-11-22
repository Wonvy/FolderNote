#pragma once
#include <windows.h>
#include <functional>
#include <optional>
#include <string>

namespace desktop_groups {
struct PreviewTarget {
  HWND owner = nullptr, focus = nullptr;
  std::wstring path;
  bool operator==(const PreviewTarget &) const = default;
};

// Intercept only the focused box's unmodified Space gesture before Seer's
// desktop hook. Never invoke IPC or activate a window inside the hook.
class PreviewKeys {
public:
  using Resolve = std::function<std::optional<PreviewTarget>()>;
  PreviewKeys(HWND receiver, UINT message, Resolve resolve);
  ~PreviewKeys();
  PreviewKeys(const PreviewKeys &) = delete;
  PreviewKeys &operator=(const PreviewKeys &) = delete;
  bool refresh();
  std::optional<std::wstring> take();
private:
  HWND receiver;
  UINT message;
  Resolve resolve;
  HHOOK hook = nullptr;
  bool held = false, captured = false, canceled = false;
  std::optional<PreviewTarget> pressed, pending;
  bool key(WPARAM event, const KBDLLHOOKSTRUCT &key);
  static LRESULT CALLBACK keyboard(int code, WPARAM event, LPARAM data);
  static thread_local PreviewKeys *active;
};
}
