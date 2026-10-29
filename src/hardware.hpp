#pragma once

#include "system.hpp"

#include <sys/statvfs.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace cyberdeck {

struct StorageDrive {
  std::string name;
  std::string model;
  std::string filesystem;
  std::string mountpoint;
  uint64_t size_bytes = 0;
  uint64_t free_bytes = 0;
  bool system = false;
  bool usb = false;
};

struct UsbDevice {
  std::string name;
  std::string manufacturer;
  std::string vendor_id;
  std::string product_id;
  std::string serial;
  std::string speed;
  std::string port;
};

struct HardwareSnapshot {
  std::vector<StorageDrive> drives;
  std::vector<UsbDevice> usb_devices;
};

inline std::string storage_size(uint64_t bytes) {
  constexpr uint64_t kib = 1024ull;
  constexpr uint64_t mib = 1024ull * 1024;
  constexpr uint64_t gib = 1024ull * mib;
  if (bytes >= gib) return std::to_string((bytes + gib / 2) / gib) + "G";
  if (bytes >= mib) return std::to_string((bytes + mib / 2) / mib) + "M";
  return std::to_string((bytes + kib / 2) / kib) + "K";
}

inline std::string hardware_name(std::string value) {
  for (char &ch : value)
    if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = '?';
  while (!value.empty() && value.back() == ' ') value.pop_back();
  return value;
}

inline std::string unescape_mount_path(const std::string &path) {
  std::string result;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '\\' && i + 3 < path.size() &&
        path[i + 1] >= '0' && path[i + 1] <= '7' &&
        path[i + 2] >= '0' && path[i + 2] <= '7' &&
        path[i + 3] >= '0' && path[i + 3] <= '7') {
      result += static_cast<char>((path[i + 1] - '0') * 64 +
                                  (path[i + 2] - '0') * 8 + path[i + 3] - '0');
      i += 3;
    } else result += path[i];
  }
  return result;
}

inline std::string physical_disk(const std::string &name) {
  namespace fs = std::filesystem;
  std::error_code error;
  const fs::path path = fs::path("/sys/class/block") / name;
  const fs::path resolved = fs::canonical(path, error);
  if (error) return {};
  if (fs::exists(path / "partition", error)) return resolved.parent_path().filename().string();
  return name;
}

inline HardwareSnapshot read_hardware_snapshot() {
  namespace fs = std::filesystem;
  HardwareSnapshot result;
  std::map<std::string, StorageDrive> mounted_drives;
  std::map<std::string, bool> mounted_disks;
  std::ifstream mounted("/proc/self/mounts");
  std::string line;
  while (std::getline(mounted, line)) {
    std::istringstream input(line);
    std::string source, target, filesystem;
    if (!(input >> source >> target >> filesystem) || source.rfind("/dev/", 0) != 0) continue;
    target = unescape_mount_path(target);
    const std::string name = fs::path(source).filename().string();
    const std::string disk = physical_disk(name);
    if (!disk.empty()) mounted_disks[disk] = true;
    auto existing = mounted_drives.find(source);
    if (existing != mounted_drives.end() && target != "/" &&
        existing->second.mountpoint.size() <= target.size()) continue;
    struct statvfs space {};
    if (::statvfs(target.c_str(), &space) != 0) continue;
    StorageDrive drive;
    drive.name = name;
    drive.mountpoint = target;
    drive.filesystem = filesystem;
    if (!disk.empty()) {
      const fs::path block = fs::path("/sys/class/block") / disk;
      drive.model = hardware_name(read_system_line((block / "device/model").string()));
      std::error_code disk_error;
      const fs::path resolved = fs::canonical(block, disk_error);
      drive.usb = !disk_error && resolved.string().find("/usb") != std::string::npos;
    }
    drive.size_bytes = static_cast<uint64_t>(space.f_blocks) * space.f_frsize;
    drive.free_bytes = static_cast<uint64_t>(space.f_bavail) * space.f_frsize;
    drive.system = target == "/";
    mounted_drives[source] = std::move(drive);
  }

  for (auto &entry : mounted_drives) result.drives.push_back(std::move(entry.second));

  std::error_code error;
  const fs::path block_dir("/sys/class/block");
  for (fs::directory_iterator it(block_dir, error), end; !error && it != end;
       it.increment(error)) {
    const fs::path path = it->path();
    if (fs::exists(path / "partition", error)) continue;
    if (!fs::exists(path / "device", error)) continue;
    const std::string name = path.filename().string();
    if (mounted_disks.count(name)) continue;
    uint64_t sectors = 0;
    try { sectors = std::stoull(read_system_line((path / "size").string())); }
    catch (...) { continue; }
    if (sectors == 0) continue;
    StorageDrive drive;
    drive.name = name;
    drive.model = hardware_name(read_system_line((path / "device/model").string()));
    drive.size_bytes = sectors * 512ull;
    const fs::path resolved = fs::canonical(path, error);
    drive.usb = !error && resolved.string().find("/usb") != std::string::npos;
    error.clear();
    result.drives.push_back(std::move(drive));
  }
  std::sort(result.drives.begin(), result.drives.end(), [](const auto &a, const auto &b) {
    if (a.system != b.system) return a.system;
    if (a.mountpoint.empty() != b.mountpoint.empty()) return !a.mountpoint.empty();
    if (!a.mountpoint.empty() && a.mountpoint != b.mountpoint)
      return a.mountpoint < b.mountpoint;
    return a.name < b.name;
  });

  error.clear();
  const fs::path usb_dir("/sys/bus/usb/devices");
  for (fs::directory_iterator it(usb_dir, error), end; !error && it != end;
       it.increment(error)) {
    const fs::path path = it->path();
    const std::string vendor = read_system_line((path / "idVendor").string());
    if (vendor.empty() || vendor == "1d6b") continue;
    const std::string product = read_system_line((path / "idProduct").string());
    UsbDevice device;
    device.name = hardware_name(read_system_line((path / "product").string()));
    device.manufacturer = hardware_name(read_system_line((path / "manufacturer").string()));
    device.vendor_id = vendor;
    device.product_id = product;
    device.serial = hardware_name(read_system_line((path / "serial").string()));
    device.speed = read_system_line((path / "speed").string());
    device.port = path.filename().string();
    if (device.name.empty()) device.name = device.manufacturer;
    if (device.name.empty()) device.name = vendor + ":" + product;
    result.usb_devices.push_back(std::move(device));
  }
  std::sort(result.usb_devices.begin(), result.usb_devices.end(),
            [](const UsbDevice &a, const UsbDevice &b) {
              return a.name == b.name ? a.port < b.port : a.name < b.name;
            });
  return result;
}

}  // namespace cyberdeck
