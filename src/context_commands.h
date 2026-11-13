#pragma once
#include "context_inventory.h"
#include <filesystem>
#include <string>
#include <vector>

namespace context {
enum class Mode { Program, Cmd, PowerShell, Url };

struct Command {
  std::wstring name, extension, program, arguments, directory, pathFilter, icon;
  Scene scene = Scene::File;
  Mode mode = Mode::Program;
  bool enabled = true, includeChildren = false;
};

bool commandsEnabled();
std::vector<Command> loadCommands();
std::wstring previewCommand(const Command &command);
void saveCommands(bool enabled, const std::vector<Command> &commands, const std::filesystem::path &exe);
void registerCommands(const std::filesystem::path &exe);
void unregisterCommands();
int runCommand(const std::wstring &token, const std::wstring &selection);
}
