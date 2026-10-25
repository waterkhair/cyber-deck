#include "audio.hpp"
#include "termux_api.hpp"

#include <algorithm>

namespace cyberdeck {
namespace {
void read_music_volume(AudioSnapshot &view) {
  const auto response = termux::run({"termux-volume"});
  auto data = termux::parse(response.output);
  view.outputs.clear();
  view.inputs.clear();
  view.available = false;
  if (response.ok && data && json_object_is_type(data.get(), json_type_array)) {
    const size_t count = json_object_array_length(data.get());
    for (size_t i = 0; i < count; ++i) {
      json_object *stream = json_object_array_get_idx(data.get(), i);
      if (termux::string(stream, "stream") != "music") continue;
      const int volume = termux::integer(stream, "volume");
      const int maximum = termux::integer(stream, "max_volume");
      if (maximum <= 0) break;
      AudioDevice device;
      device.name = "android-music";
      device.description = "System music";
      device.volume = static_cast<unsigned>(100 * std::clamp(volume, 0, maximum) / maximum);
      device.is_default = true;
      view.outputs.push_back(std::move(device));
      view.available = true;
      break;
    }
  }
  view.status = view.available ? "Android music volume" :
      response.ok ? "Music volume unavailable" : response.error;
}
}  // namespace

struct AudioBackend::Impl {
  termux::Worker<AudioSnapshot> worker{[] {
    AudioSnapshot value;
    value.volume_only = true;
    value.status = "Loading Termux:API volume...";
    return value;
  }()};
};

AudioBackend::AudioBackend() : impl_(std::make_unique<Impl>()) {}
AudioBackend::~AudioBackend() = default;
int AudioBackend::fd() const { return impl_->worker.fd(); }
AudioSnapshot AudioBackend::snapshot() { return impl_->worker.snapshot(); }
void AudioBackend::refresh() {
  impl_->worker.post(read_music_volume, [](AudioSnapshot &view) {
    view.status = "Refreshing Android music volume...";
  });
}
void AudioBackend::set_default(bool, const std::string &) {}

void AudioBackend::set_volume(bool output, const std::string &name, unsigned percent) {
  if (!output || name != "android-music") return;
  impl_->worker.post([percent](AudioSnapshot &view) {
    const auto current = termux::run({"termux-volume"});
    auto data = termux::parse(current.output);
    int maximum = 0;
    if (current.ok && data && json_object_is_type(data.get(), json_type_array)) {
      const size_t count = json_object_array_length(data.get());
      for (size_t i = 0; i < count; ++i) {
        json_object *stream = json_object_array_get_idx(data.get(), i);
        if (termux::string(stream, "stream") == "music") {
          maximum = termux::integer(stream, "max_volume");
          break;
        }
      }
    }
    if (maximum <= 0) { view.status = "Cannot read Android music volume"; return; }
    const unsigned step = (std::min(percent, 100u) * static_cast<unsigned>(maximum) + 50u) / 100u;
    const auto changed = termux::run({"termux-volume", "music", std::to_string(step)});
    if (!changed.ok) { view.status = changed.error; return; }
    read_music_volume(view);
  });
}

}  // namespace cyberdeck
