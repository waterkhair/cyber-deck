#pragma once

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace cyberdeck {

struct SystemOverview {
  std::string load = "Unavailable";
  std::string memory = "Unavailable";
  std::string uptime = "Unavailable";
  std::string network = "No active link";
  std::string battery = "Not detected";
};

inline std::string read_system_line(const std::string &path) {
  std::ifstream file(path);
  std::string value;
  if (file) std::getline(file, value);
  return value;
}

inline SystemOverview read_system_overview() {
  SystemOverview result;
  {
    std::ifstream file("/proc/loadavg");
    std::string one, five, fifteen;
    if (file >> one >> five >> fifteen)
      result.load = one + "  " + five + "  " + fifteen;
  }
  {
    std::ifstream file("/proc/meminfo");
    std::string line, key;
    uint64_t total = 0, available = 0, amount = 0;
    while (std::getline(file, line)) {
      std::istringstream entry(line);
      if (!(entry >> key >> amount)) continue;
      if (key == "MemTotal:") total = amount;
      if (key == "MemAvailable:") available = amount;
      if (total && available) break;
    }
    if (total && available) {
      std::ostringstream text;
      text << std::fixed << std::setprecision(1)
           << (total - available) / 1048576.0 << " / " << total / 1048576.0 << " GiB";
      result.memory = text.str();
    }
  }
  {
    std::ifstream file("/proc/uptime");
    double seconds = 0;
    if (file >> seconds) {
      const auto minutes = static_cast<uint64_t>(seconds / 60);
      const auto days = minutes / 1440;
      const auto hours = minutes / 60 % 24;
      const auto remainder = minutes % 60;
      result.uptime = (days ? std::to_string(days) + "d " : "") +
                      std::to_string(hours) + "h " + std::to_string(remainder) + "m";
    }
  }
  {
    std::string default_interface;
    std::ifstream routes("/proc/net/route");
    std::string line, destination, interface;
    (void)std::getline(routes, line);
    while (std::getline(routes, line)) {
      std::istringstream entry(line);
      if (entry >> interface >> destination && destination == "00000000") {
        default_interface = interface;
        break;
      }
    }
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
      std::string fallback;
      for (ifaddrs *item = interfaces; item; item = item->ifa_next) {
        if (!item->ifa_name || !item->ifa_addr || (item->ifa_flags & IFF_LOOPBACK) ||
            !(item->ifa_flags & IFF_UP) || !(item->ifa_flags & IFF_RUNNING) ||
            (item->ifa_addr->sa_family != AF_INET && item->ifa_addr->sa_family != AF_INET6))
          continue;
        if (fallback.empty()) fallback = item->ifa_name;
        if (default_interface == item->ifa_name) {
          fallback = item->ifa_name;
          break;
        }
      }
      if (!fallback.empty()) result.network = fallback + " link up";
      freeifaddrs(interfaces);
    }
  }
  {
    DIR *directory = opendir("/sys/class/power_supply");
    if (directory) {
      while (dirent *entry = readdir(directory)) {
        if (entry->d_name[0] == '.') continue;
        const std::string base = std::string("/sys/class/power_supply/") + entry->d_name;
        if (read_system_line(base + "/type") != "Battery") continue;
        const std::string capacity = read_system_line(base + "/capacity");
        const std::string state = read_system_line(base + "/status");
        result.battery = capacity.empty() ? "Present" : capacity + "%";
        if (!state.empty()) result.battery += "  " + state;
        break;
      }
      closedir(directory);
    }
  }
  return result;
}

}  // namespace cyberdeck
