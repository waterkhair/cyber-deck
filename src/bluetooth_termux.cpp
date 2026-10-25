#include "bluetooth.hpp"
#include "termux_api.hpp"

namespace cyberdeck {

struct BluetoothBackend::Impl {
  termux::Worker<BluetoothSnapshot> worker{[] {
    BluetoothSnapshot value;
    value.info_only = true;
    value.status = "Loading Bluetooth audio info...";
    return value;
  }()};
};

BluetoothBackend::BluetoothBackend() : impl_(std::make_unique<Impl>()) {}
BluetoothBackend::~BluetoothBackend() = default;
int BluetoothBackend::fd() const { return impl_->worker.fd(); }
BluetoothSnapshot BluetoothBackend::snapshot() { return impl_->worker.snapshot(); }
void BluetoothBackend::refresh() {
  impl_->worker.post([](BluetoothSnapshot &view) {
    const auto response = termux::run({"termux-audio-info"});
    auto data = termux::parse(response.output);
    view.available = response.ok && data && json_object_is_type(data.get(), json_type_object) &&
        termux::field(data.get(), "BLUETOOTH_A2DP_IS_ON");
    view.status = view.available ?
        (termux::boolean(data.get(), "BLUETOOTH_A2DP_IS_ON") ?
            "Bluetooth audio route active" : "No Bluetooth audio route active") :
        response.ok ? "Bluetooth audio info unavailable" : response.error;
  }, [](BluetoothSnapshot &view) {
    view.status = "Refreshing Bluetooth audio route...";
  });
}
void BluetoothBackend::set_powered(bool) {}
void BluetoothBackend::set_discovering(bool) {}
void BluetoothBackend::pair(const std::string &, bool) {}
void BluetoothBackend::connect(const std::string &) {}
void BluetoothBackend::disconnect(const std::string &) {}
void BluetoothBackend::forget(const std::string &) {}
void BluetoothBackend::set_trusted(const std::string &, bool) {}
void BluetoothBackend::answer_prompt(uint64_t, bool, std::string) {}

}  // namespace cyberdeck
