#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace cyberdeck {

struct UiPreferences {
  bool vi_keys = false;
  std::array<bool, 4> dashboard_widgets{{true, true, true, true}};
  bool unicode_glyphs = true;
};

inline std::filesystem::path ui_preferences_path() {
  const char *config = std::getenv("XDG_CONFIG_HOME");
  if (config && *config && std::filesystem::path(config).is_absolute())
    return std::filesystem::path(config) / "cyber-deck" / "ui";
  const char *home = std::getenv("HOME");
  if (home && *home) return std::filesystem::path(home) / ".config/cyber-deck/ui";
  return {};
}

inline UiPreferences load_ui_preferences() {
  UiPreferences prefs;
  const auto path = ui_preferences_path();
  if (path.empty()) return prefs;
  std::ifstream input(path);
  int vi = 0, clock = 1, weather = 1, network = 1, battery = 1, unicode = 1;
  if (input >> vi >> clock >> weather >> network >> battery) {
    prefs.vi_keys = vi != 0;
    prefs.dashboard_widgets = {{clock != 0, weather != 0, network != 0, battery != 0}};
    if (input >> unicode) prefs.unicode_glyphs = unicode != 0;
  }
  return prefs;
}

inline bool save_ui_preferences(const UiPreferences &prefs) {
  try {
    const auto path = ui_preferences_path();
    if (path.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << (prefs.vi_keys ? 1 : 0);
    for (bool enabled : prefs.dashboard_widgets) output << ' ' << (enabled ? 1 : 0);
    output << ' ' << (prefs.unicode_glyphs ? 1 : 0);
    output << '\n';
    output.flush();
    return output.good();
  } catch (const std::filesystem::filesystem_error &) {
    return false;
  }
}

}  // namespace cyberdeck
