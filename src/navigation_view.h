#pragma once
#include "navigation.h"
#include <shobjidl.h>
namespace viewnav {
struct Controller;
// All controller calls belong to the browser/tab's UI thread. On Win11 that
// thread need not own the top-level Explorer window.
// Choose a stable same-thread host for this browser without collapsing tabs.
HWND attachmentWindow(HWND frame, HWND browserWindow, HWND view);
Controller *attach(HWND host, IShellBrowser *browser);
void refresh(Controller *, IShellBrowser *browser);
void detach(Controller *);
bool hasView(const Controller *);
long activeControllers();
long activeQueryWorkers();
// The caller has already validated foreground, gesture and blank surface.
// Recheck the active Shell view before any navigation, including Win11 tabs.
HRESULT browse(IShellBrowser *, HWND expectedView, navigation::Action);
} // namespace viewnav
