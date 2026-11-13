#pragma once
#include <string>
#include <vector>

namespace this_pc_view {
enum class Group { Folders, Drives, Network };
struct Item {
  std::wstring id, name, target;
  Group group = Group::Folders;
  int iconIndex = -1;
};

std::vector<Item> items();
Group classify(const std::wstring &id);
}
