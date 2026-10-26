#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cyberdeck {

enum class WifiSecurity { Open, Personal, Enterprise, Unsupported };

struct WifiNetwork {
  std::string ssid;
  std::string path;
  std::string saved_uuid;
  bool visible = true;
  unsigned strength = 0;
  WifiSecurity security = WifiSecurity::Unsupported;
  bool uses_sae = false;
  bool connected = false;
};

struct LanConnection {
  std::string path;
  std::string interface_name;
  std::string profile;
  std::string ip_address;
  bool managed = false;
  bool carrier = false;
  bool connected = false;
  bool connecting = false;
  unsigned speed_mbps = 0;
};

struct WifiSnapshot {
  bool available = false;
  bool enabled = false;
  bool hardware_enabled = true;
  bool scanning = false;
  bool connecting = false;
  bool forgetting = false;
  uint64_t forget_epoch = 0;
  std::string device;
  std::string connected_ssid;
  std::string status = "NetworkManager unavailable";
  std::vector<LanConnection> wired;
  std::vector<WifiNetwork> networks;
};

class WifiBackend {
 public:
  WifiBackend();
  ~WifiBackend();
  WifiBackend(const WifiBackend &) = delete;
  WifiBackend &operator=(const WifiBackend &) = delete;

  int fd() const;
  WifiSnapshot snapshot();
  void scan();
  void connect(const WifiNetwork &network, std::string password = {});
  void disconnect();
  void forget(const std::string &uuid);
  void set_enabled(bool enabled);
  void connect_lan(const std::string &path);
  void disconnect_lan(const std::string &path);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cyberdeck
