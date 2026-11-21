#pragma once
#include <windows.h>
#include "quick_find_settings.h"
namespace quick_find::external {
bool enabled();
void saveEnabled(bool enabled);
bool start();
void stop();
// Target is validated as Explorer or a system file dialog by the existing helper.
void target(HWND window);
bool keyboard(WPARAM message, const KBDLLHOOKSTRUCT &key);
void mouse(WPARAM message, const MSLLHOOKSTRUCT &event);
void cancel();
}
