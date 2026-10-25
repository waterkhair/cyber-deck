#include "wifi.hpp"

namespace cyberdeck {
struct WifiBackend::Impl {};
WifiBackend::WifiBackend() : impl_(new Impl) {}
WifiBackend::~WifiBackend() = default;
int WifiBackend::fd() const { return -1; }
WifiSnapshot WifiBackend::snapshot() { return {}; }
void WifiBackend::scan() {}
void WifiBackend::connect(const WifiNetwork &, std::string) {}
void WifiBackend::disconnect() {}
void WifiBackend::forget(const std::string &) {}
void WifiBackend::set_enabled(bool) {}
}  // namespace cyberdeck
