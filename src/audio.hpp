#pragma once

#include <memory>
#include <string>
#include <vector>

namespace cyberdeck {

struct AudioDevice {
  std::string name;
  std::string description;
  unsigned volume = 0;
  unsigned channels = 0;
  bool muted = false;
  bool is_default = false;
};

struct AudioSnapshot {
  bool available = false;
  std::string status = "Audio unavailable";
  std::vector<AudioDevice> outputs;
  std::vector<AudioDevice> inputs;
};

class AudioBackend {
 public:
  AudioBackend();
  ~AudioBackend();
  AudioBackend(const AudioBackend &) = delete;
  AudioBackend &operator=(const AudioBackend &) = delete;

  int fd() const;
  AudioSnapshot snapshot();
  void refresh();
  void set_default(bool output, const std::string &name);
  void set_volume(bool output, const std::string &name, unsigned percent);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cyberdeck
