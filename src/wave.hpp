#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace cyberdeck {

struct WaveStation {
  std::string name;
  std::string url;
};

// Keep the bundled station list in sync with cyber-wave's default configuration.
inline constexpr std::array<std::pair<const char *, const char *>, 3> kDefaultWaveStations = {{
    {"Esoterica Radio S3",
     "https://esoterica.servemp3.com:444/listen/darkbasshouse_cyberpunk_hybridtrap/radio.mp3"},
    {"Nightride FM - Darksynth", "https://stream.nightride.fm/darksynth.mp3"},
    {"REYFM - LOFI", "https://listen.reyfm.de/lofi_320kbps.mp3"},
}};

inline constexpr size_t kNoWaveStation = static_cast<size_t>(-1);

struct WaveSnapshot {
  std::string status = "Stopped";
  size_t playing = kNoWaveStation;
};

class WaveBackend {
 public:
  ~WaveBackend();
  WaveBackend(const WaveBackend &) = delete;
  WaveBackend &operator=(const WaveBackend &) = delete;
  WaveBackend();

  const WaveSnapshot &snapshot() const { return view_; }
  const std::vector<WaveStation> &stations() const { return stations_; }
  bool add_station(const std::string &name, const std::string &url, std::string &error);
  bool tick();
  void toggle(size_t index);
  void stop();

 private:
  void terminate_active();
  bool save_stations(std::string &error) const;

  WaveSnapshot view_;
  std::vector<WaveStation> stations_;
  pid_t active_pid_ = -1;
  std::vector<pid_t> retiring_;
};

}  // namespace cyberdeck
