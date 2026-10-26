#pragma once

#include <memory>
#include <string>

namespace cyberdeck {

struct WeatherSnapshot {
  std::string location;
  std::string temperature;
  std::string condition;
  std::string status = "Set city";
  bool loading = false;
};

class WeatherBackend {
 public:
  WeatherBackend();
  ~WeatherBackend();
  WeatherBackend(const WeatherBackend &) = delete;
  WeatherBackend &operator=(const WeatherBackend &) = delete;

  int fd() const;
  WeatherSnapshot snapshot();
  void refresh();
  bool set_location(const std::string &location);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cyberdeck
