#include "weather.hpp"

namespace cyberdeck {

struct WeatherBackend::Impl {
  WeatherSnapshot view;
};

WeatherBackend::WeatherBackend() : impl_(new Impl) {
  impl_->view.status = "Weather support unavailable";
}
WeatherBackend::~WeatherBackend() = default;
int WeatherBackend::fd() const { return -1; }
WeatherSnapshot WeatherBackend::snapshot() { return impl_->view; }
void WeatherBackend::refresh() {}
bool WeatherBackend::set_location(const std::string &location) {
  impl_->view.location = location;
  return false;
}

}  // namespace cyberdeck
