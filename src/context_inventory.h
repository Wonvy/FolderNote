#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context {
enum class Scene { File, Extension, Folder, FolderBackground, Drive, Desktop, New, SendTo, OpenWith };

struct ExistingItem {
  std::wstring label, source, icon, command;
  std::wstring program, arguments;
  bool canHide = false, hidden = false, managed = false, canEdit = false;

  std::wstring registryKey;
  bool machine = false;
  bool sendTo = false;
  std::uint32_t fileVolume = 0;
  std::uint64_t fileIndex = 0, fileCreated = 0, fileWritten = 0;
  bool shellBlock = false;
  bool handlerRegistration = false;
  bool machineOverlay = false;
  std::wstring shellBlockId, shellBlockModule;
  std::wstring registeredLabel;
  int noteOperation = -1;
  std::vector<std::wstring> cascadeChildren;
  std::wstring cascadeTarget;
  std::vector<ExistingItem> children;
};

std::vector<ExistingItem> inventory(Scene scene, std::wstring_view extension = {});
void setHidden(const ExistingItem &item, bool hidden);
// An empty field restores the original registry value if ShibaKit changed it.
void setAppearance(const ExistingItem &item, std::wstring_view label, std::wstring_view icon);
void restore(const ExistingItem &item);
// Best-effort restoration. The result counts records still requiring attention.
size_t restoreAll();
}
