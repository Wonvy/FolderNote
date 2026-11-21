#pragma once
#include <windows.h>

namespace quick_find {
struct Settings {
  // The Explorer scope also covers the native Windows desktop file view.
  bool boxes = true, lists = true, explorer = false, dialogs = false;
  bool openFolders = true, openFiles = false, atPointer = true, showIcons = true;
  unsigned visibleRows = 9;
  bool opens(bool folder, bool fileDialog = false) const {
    return folder ? openFolders : openFiles && !fileDialog;
  }
  bool operator==(const Settings &) const = default;
};
Settings settings();
// One registry value makes each settings change atomic across processes.
LSTATUS saveSettings(const Settings &value);
}
