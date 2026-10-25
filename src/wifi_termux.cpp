#include "wifi.hpp"
#include "termux_api.hpp"

#include <algorithm>

namespace cyberdeck {

struct WifiBackend::Impl {
  termux::Worker<WifiSnapshot> worker{[] {
    WifiSnapshot value;
    value.read_only = true;
    value.device = "Android WiFi";
    value.status = "Loading Termux:API WiFi info...";
    return value;
  }()};
};

WifiBackend::WifiBackend() : impl_(std::make_unique<Impl>()) {}
WifiBackend::~WifiBackend() = default;
int WifiBackend::fd() const { return impl_->worker.fd(); }
WifiSnapshot WifiBackend::snapshot() { return impl_->worker.snapshot(); }

void WifiBackend::scan() {
  impl_->worker.post([](WifiSnapshot &view) {
    const auto connection = termux::run({"termux-wifi-connectioninfo"});
    if (!connection.ok) {
      view.scanning = false;
      view.available = false;
      view.status = connection.error;
      return;
    }
    const auto nearby = termux::run({"termux-wifi-scaninfo"});
    auto info = termux::parse(connection.output);
    auto scans = termux::parse(nearby.output);
    view.connected_ssid.clear();
    view.details.clear();
    view.networks.clear();
    view.scanning = false;
    view.available = false;
    view.enabled = false;
    if (connection.ok && info && json_object_is_type(info.get(), json_type_object) &&
        termux::string(info.get(), "API_ERROR").empty()) {
      view.available = true;
      view.enabled = true;
      view.connected_ssid = termux::string(info.get(), "ssid");
      if (view.connected_ssid == "<unknown ssid>") view.connected_ssid.clear();
      const std::string ip = termux::string(info.get(), "ip");
      if (!ip.empty() && ip != "0.0.0.0") view.details = "IP " + ip;
      const int rssi = termux::integer(info.get(), "rssi", 0);
      if (rssi < 0) {
        if (!view.details.empty()) view.details += "  ";
        view.details += "Signal " + std::to_string(rssi) + " dBm";
      }
    }
    if (nearby.ok && scans && json_object_is_type(scans.get(), json_type_array)) {
      view.available = true;
      view.enabled = true;
      const size_t count = std::min<size_t>(json_object_array_length(scans.get()), 128);
      for (size_t i = 0; i < count; ++i) {
        json_object *item = json_object_array_get_idx(scans.get(), i);
        const std::string ssid = termux::string(item, "ssid");
        if (ssid.empty()) continue;
        WifiNetwork network;
        network.ssid = ssid;
        network.path = termux::string(item, "bssid");
        const int rssi = termux::integer(item, "rssi", -100);
        network.strength = static_cast<unsigned>(std::clamp(2 * (rssi + 100), 0, 100));
        network.connected = !view.connected_ssid.empty() && ssid == view.connected_ssid;
        const std::string capability = termux::string(item, "capabilities");
        if (capability.find("EAP") != std::string::npos)
          network.security = WifiSecurity::Enterprise;
        else if (capability.find("WPA") != std::string::npos ||
                 capability.find("RSN") != std::string::npos ||
                 capability.find("SAE") != std::string::npos)
          network.security = WifiSecurity::Personal;
        else if (capability.find("WEP") != std::string::npos)
          network.security = WifiSecurity::Unsupported;
        else network.security = WifiSecurity::Open;
        view.networks.push_back(std::move(network));
      }
    }
    if (view.available) {
      const std::string scan_error = termux::string(scans.get(), "API_ERROR");
      view.status = scan_error.empty() ? "Android WiFi info; scan may be cached" : scan_error;
    } else {
      view.status = !connection.ok ? connection.error : !nearby.ok ? nearby.error :
          "Grant Termux:API location access for WiFi scan";
    }
  }, [](WifiSnapshot &view) {
    view.scanning = true;
    view.status = "Refreshing Android WiFi info...";
  });
}

void WifiBackend::connect(const WifiNetwork &, std::string) {}
void WifiBackend::disconnect() {}
void WifiBackend::forget(const std::string &) {}
void WifiBackend::set_enabled(bool) {}

}  // namespace cyberdeck
