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
  std::vector<WifiNetwork> networks;
  bool read_only = false;
  std::string details;
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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cyberdeck
