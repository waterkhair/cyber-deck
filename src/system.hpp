#pragma once

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace cyberdeck {

struct SystemOverview {
  std::string network_interface;
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
    std::string default_interface;
    unsigned best_metric = std::numeric_limits<unsigned>::max();
    std::ifstream routes("/proc/net/route");
    std::string line, destination, interface, gateway, flags, refcount, uses;
    (void)std::getline(routes, line);
    while (std::getline(routes, line)) {
      std::istringstream entry(line);
      unsigned metric = 0;
      if (entry >> interface >> destination >> gateway >> flags >> refcount >> uses >> metric &&
          destination == "00000000" &&
          (default_interface.empty() || metric < best_metric)) {
        default_interface = interface;
        best_metric = metric;
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
      if (!fallback.empty()) result.network_interface = fallback;
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
