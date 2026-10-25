#include "bluetooth.hpp"

namespace cyberdeck {

struct BluetoothBackend::Impl {};
BluetoothBackend::BluetoothBackend() : impl_(std::make_unique<Impl>()) {}
BluetoothBackend::~BluetoothBackend() = default;
int BluetoothBackend::fd() const { return -1; }
BluetoothSnapshot BluetoothBackend::snapshot() { return {}; }
void BluetoothBackend::refresh() {}
void BluetoothBackend::set_powered(bool) {}
void BluetoothBackend::set_discovering(bool) {}
void BluetoothBackend::pair(const std::string &, bool) {}
void BluetoothBackend::connect(const std::string &) {}
void BluetoothBackend::disconnect(const std::string &) {}
void BluetoothBackend::forget(const std::string &) {}
void BluetoothBackend::set_trusted(const std::string &, bool) {}
void BluetoothBackend::answer_prompt(uint64_t, bool, std::string) {}

}  // namespace cyberdeck
