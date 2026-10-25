#include "audio.hpp"

namespace cyberdeck {
struct AudioBackend::Impl {};
AudioBackend::AudioBackend() : impl_(new Impl) {}
AudioBackend::~AudioBackend() = default;
int AudioBackend::fd() const { return -1; }
AudioSnapshot AudioBackend::snapshot() { return {}; }
void AudioBackend::refresh() {}
void AudioBackend::set_default(bool, const std::string &) {}
void AudioBackend::set_volume(bool, const std::string &, unsigned) {}
}  // namespace cyberdeck
