#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace cyberdeck {

enum class CookiePolicy : size_t { Block, Ask, AllowSession };

constexpr std::array<const char *, 3> kCookiePolicyNames = {
    "Block all", "Ask", "Allow for session"};

inline std::filesystem::path cookie_policy_path() {
  const char *config = std::getenv("XDG_CONFIG_HOME");
  if (config && *config && std::filesystem::path(config).is_absolute())
    return std::filesystem::path(config) / "cyber-deck" / "cookies";
  const char *home = std::getenv("HOME");
  if (home && *home)
    return std::filesystem::path(home) / ".config" / "cyber-deck" / "cookies";
  return {};
}

inline CookiePolicy load_cookie_policy() {
  const auto path = cookie_policy_path();
  if (path.empty()) return CookiePolicy::Block;
  std::ifstream input(path);
  std::string name;
  std::getline(input, name);
  for (size_t i = 0; i < kCookiePolicyNames.size(); ++i)
    if (name == kCookiePolicyNames[i]) return static_cast<CookiePolicy>(i);
  return CookiePolicy::Block;
}

inline bool save_cookie_policy(CookiePolicy policy) {
  try {
    const auto path = cookie_policy_path();
    if (path.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << kCookiePolicyNames[static_cast<size_t>(policy)] << '\n';
    output.flush();
    return output.good();
  } catch (const std::filesystem::filesystem_error &) {
    return false;
  }
}

}  // namespace cyberdeck
