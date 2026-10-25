#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace cyberdeck {

inline std::filesystem::path notes_directory() {
  const char *data = std::getenv("XDG_DATA_HOME");
  if (data && *data && data[0] == '/')
    return std::filesystem::path(data) / "cyber-deck" / "notes";
  const char *home = std::getenv("HOME");
  if (home && *home && home[0] == '/')
    return std::filesystem::path(home) / ".local/share/cyber-deck/notes";
  return {};
}

inline std::vector<std::string> list_notes(const std::filesystem::path &directory) {
  std::vector<std::string> names;
  if (directory.empty()) return names;
  std::error_code error;
  std::filesystem::directory_iterator entries(directory, error);
  if (error) return names;
  for (const auto &entry : entries) {
    if (entry.path().extension() == ".txt" &&
        std::filesystem::is_regular_file(entry.symlink_status(error)) && !error)
      names.push_back(entry.path().filename().string());
    error.clear();
  }
  std::sort(names.begin(), names.end());
  return names;
}

inline bool valid_note_name(const std::string &name) {
  if (name.empty() || name.size() > 100 || name == "." || name == ".." || name[0] == '.')
    return false;
  return std::none_of(name.begin(), name.end(), [](unsigned char ch) {
    return ch < 32 || ch == 127 || ch == '/' || ch == '\\';
  });
}

inline std::string find_executable(const std::string &name) {
  const char *path = std::getenv("PATH");
  if (!path) return {};
  std::string search(path);
  size_t start = 0;
  while (start <= search.size()) {
    const size_t end = search.find(':', start);
    const std::string directory = search.substr(start, end == std::string::npos ? end : end - start);
    if (!directory.empty()) {
      const auto candidate = std::filesystem::path(directory) / name;
      std::error_code error;
      if (std::filesystem::is_regular_file(candidate, error) && !error &&
          ::access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

inline std::string find_nano() { return find_executable("nano"); }

}  // namespace cyberdeck
