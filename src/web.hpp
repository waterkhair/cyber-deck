#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace cyberdeck {

inline std::filesystem::path web_header_links_path() {
  const char *config = std::getenv("XDG_CONFIG_HOME");
  if (config && *config && std::filesystem::path(config).is_absolute())
    return std::filesystem::path(config) / "cyber-deck" / "web-header-links";
  const char *home = std::getenv("HOME");
  if (home && *home)
    return std::filesystem::path(home) / ".config" / "cyber-deck" / "web-header-links";
  return {};
}

inline bool load_web_header_links() {
  const auto path = web_header_links_path();
  if (path.empty()) return false;
  std::ifstream input(path);
  std::string value;
  std::getline(input, value);
  return value == "on";
}

inline bool save_web_header_links(bool enabled) {
  try {
    const auto path = web_header_links_path();
    if (path.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << (enabled ? "on" : "off") << '\n';
    output.flush();
    return output.good();
  } catch (const std::filesystem::filesystem_error &) {
    return false;
  }
}

inline std::string encode_web_query(const std::string &query) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char ch : query) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
      encoded.push_back(static_cast<char>(ch));
    else {
      encoded.push_back('%');
      encoded.push_back(hex[ch >> 4]);
      encoded.push_back(hex[ch & 15]);
    }
  }
  return encoded;
}

}  // namespace cyberdeck
