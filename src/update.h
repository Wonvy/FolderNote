#pragma once
#include <optional>
#include <string>

namespace updater {
struct Release {
  std::wstring version;
  std::wstring downloadUrl;
  std::wstring sha256;
  bool win11Package = false;
  bool standaloneInstaller = false;
  std::wstring notes;
};

// Format raw GitHub release Markdown into clean plain text for the update confirmation dialog.
std::wstring formatReleaseNotes(std::wstring_view markdown);

// Parse release metadata separately so package selection can be verified offline.
std::optional<Release> parseRelease(const std::string &content, const std::wstring &repository,
                                   const std::wstring &currentVersion, bool win11Package = false);

// These operations perform network and disk I/O. Call them off the UI thread.
std::optional<Release> check(const std::wstring &repository, const std::wstring &currentVersion,
                             bool win11Package = false);
// A supplied predicate selects automatic installation and is rechecked before launch.
// Returns false if the automatic update no longer applies.
bool install(const Release &release, bool (*allowAutomaticInstall)() = nullptr);
}
