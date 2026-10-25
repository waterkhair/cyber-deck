#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace cyberdeck {

enum class Theme : size_t { Husky, Synthwave, Greenline };

constexpr std::array<const char *, 3> kThemeNames = {
    "Husky", "Synthwave", "Greenline"};

inline const char *theme_name(Theme theme) {
  return kThemeNames[static_cast<size_t>(theme)];
}

inline std::filesystem::path theme_path() {
  const char *config = std::getenv("XDG_CONFIG_HOME");
  if (config && *config && std::filesystem::path(config).is_absolute())
    return std::filesystem::path(config) / "cyber-deck" / "theme";
  const char *home = std::getenv("HOME");
  if (home && *home)
    return std::filesystem::path(home) / ".config" / "cyber-deck" / "theme";
  return {};
}

inline Theme load_theme() {
  const auto path = theme_path();
  if (path.empty()) return Theme::Husky;
  std::ifstream input(path);
  std::string name;
  std::getline(input, name);
  for (size_t i = 0; i < kThemeNames.size(); ++i)
    if (name == kThemeNames[i]) return static_cast<Theme>(i);
  return Theme::Husky;
}

inline bool save_theme(Theme theme) {
  try {
    const auto path = theme_path();
    if (path.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << theme_name(theme) << '\n';
    output.flush();
    return output.good();
  } catch (const std::filesystem::filesystem_error &) {
    return false;
  }
}

}  // namespace cyberdeck
