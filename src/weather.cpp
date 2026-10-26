#include "weather.hpp"

#include <curl/curl.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace cyberdeck {
namespace {

std::filesystem::path location_path() {
  const char *config = std::getenv("XDG_CONFIG_HOME");
  if (config && *config && std::filesystem::path(config).is_absolute())
    return std::filesystem::path(config) / "cyber-deck" / "weather-location";
  const char *home = std::getenv("HOME");
  return home && *home ? std::filesystem::path(home) / ".config/cyber-deck/weather-location" :
                         std::filesystem::path{};
}

std::string load_location() {
  std::ifstream input(location_path());
  std::string location;
  if (input) std::getline(input, location);
  return location.substr(0, 100);
}

bool save_location(const std::string &location) {
  try {
    const auto path = location_path();
    if (path.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    output << location << '\n';
    return output.good();
  } catch (const std::filesystem::filesystem_error &) {
    return false;
  }
}

std::optional<double> json_number(const std::string &json, std::string_view key,
                                  size_t after = 0) {
  const std::string quoted = "\"" + std::string(key) + "\"";
  const size_t name = json.find(quoted, after);
  if (name == std::string::npos) return std::nullopt;
  const size_t colon = json.find(':', name + quoted.size());
  if (colon == std::string::npos) return std::nullopt;
  const char *start = json.c_str() + colon + 1;
  char *end = nullptr;
  const double value = std::strtod(start, &end);
  if (end == start || !std::isfinite(value)) return std::nullopt;
  return value;
}

const char *condition_name(int code) {
  if (code == 0) return "Clear sky";
  if (code == 1) return "Mostly clear";
  if (code == 2) return "Partly cloudy";
  if (code == 3) return "Cloudy";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Rain showers";
  if (code == 85 || code == 86) return "Snow showers";
  if (code >= 95 && code <= 99) return "Thunderstorm";
  return "Conditions unknown";
}

size_t append_response(char *data, size_t size, size_t count, void *user) {
  auto &response = *static_cast<std::string *>(user);
  const size_t bytes = size * count;
  if (bytes > 65536 - std::min(response.size(), size_t{65536})) return 0;
  response.append(data, bytes);
  return bytes;
}

std::optional<std::string> fetch(CURL *curl, const std::string &url) {
  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 6000L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "CyberDeck/1.0 (+https://github.com/WaterKhair/cyber-deck)");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  if (curl_easy_perform(curl) != CURLE_OK) return std::nullopt;
  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  if (http_status != 200) return std::nullopt;
  return response;
}

WeatherSnapshot query_weather(CURL *curl, const std::string &location) {
  WeatherSnapshot result;
  result.location = location;
  result.status = "Weather unavailable";
  char *escaped = curl_easy_escape(curl, location.c_str(), static_cast<int>(location.size()));
  if (!escaped) return result;
  const std::string geocode_url =
      "https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + std::string(escaped);
  curl_free(escaped);
  const auto geocode = fetch(curl, geocode_url);
  if (!geocode) return result;
  const size_t results = geocode->find("\"results\"");
  if (results == std::string::npos) {
    result.status = "City not found";
    return result;
  }
  const auto latitude = json_number(*geocode, "latitude", results);
  const auto longitude = json_number(*geocode, "longitude", results);
  if (!latitude || !longitude || *latitude < -90 || *latitude > 90 ||
      *longitude < -180 || *longitude > 180) return result;
  const std::string forecast_url = "https://api.open-meteo.com/v1/forecast?latitude=" +
      std::to_string(*latitude) + "&longitude=" + std::to_string(*longitude) +
      "&current=temperature_2m,weather_code&forecast_days=1";
  const auto forecast = fetch(curl, forecast_url);
  if (!forecast) return result;
  const size_t current = forecast->find("\"current\"");
  if (current == std::string::npos) return result;
  const auto temperature = json_number(*forecast, "temperature_2m", current);
  const auto code = json_number(*forecast, "weather_code", current);
  if (!temperature || !code || *temperature < -100 || *temperature > 100) return result;
  result.temperature = std::to_string(std::lround(*temperature)) + "\xC2\xB0" "C";
  result.condition = condition_name(static_cast<int>(*code));
  result.status = "Updated";
  return result;
}

void make_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  const int descriptor_flags = fcntl(fd, F_GETFD, 0);
  if (descriptor_flags >= 0) (void)fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
}

}  // namespace

struct WeatherBackend::Impl {
  int pipe_fd[2] = {-1, -1};
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  WeatherSnapshot view;
  bool stopping = false;
  bool pending = false;

  Impl() {
    if (::pipe(pipe_fd) != 0) throw std::runtime_error("cannot create weather notification pipe");
    make_nonblocking(pipe_fd[0]);
    make_nonblocking(pipe_fd[1]);
    view.location = load_location();
    pending = !view.location.empty();
    if (pending) {
      view.loading = true;
      view.status = "Loading weather...";
    }
    try { worker = std::thread([this] { run(); }); }
    catch (...) {
      ::close(pipe_fd[0]);
      ::close(pipe_fd[1]);
      throw;
    }
  }

  ~Impl() {
    { std::lock_guard<std::mutex> lock(mutex); stopping = true; }
    wake.notify_one();
    if (worker.joinable()) worker.join();
    ::close(pipe_fd[0]);
    ::close(pipe_fd[1]);
  }

  void notify() {
    const char byte = 1;
    (void)::write(pipe_fd[1], &byte, 1);
  }

  void run() {
    const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    CURL *curl = ready ? curl_easy_init() : nullptr;
    for (;;) {
      std::string location;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [this] { return pending || stopping; });
        if (stopping) break;
        pending = false;
        location = view.location;
        if (location.empty()) continue;
        view.loading = true;
        view.status = "Loading weather...";
        notify();
      }
      WeatherSnapshot result;
      if (curl) result = query_weather(curl, location);
      else { result.location = location; result.status = "Weather unavailable"; }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (view.location != location) continue;
        view = std::move(result);
        notify();
      }
    }
    if (curl) curl_easy_cleanup(curl);
    if (ready) curl_global_cleanup();
  }
};

WeatherBackend::WeatherBackend() : impl_(new Impl) {}
WeatherBackend::~WeatherBackend() = default;
int WeatherBackend::fd() const { return impl_->pipe_fd[0]; }
WeatherSnapshot WeatherBackend::snapshot() {
  char buffer[64];
  while (::read(impl_->pipe_fd[0], buffer, sizeof(buffer)) > 0) {}
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->view;
}
void WeatherBackend::refresh() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->view.location.empty()) return;
  impl_->pending = true;
  impl_->wake.notify_one();
}
bool WeatherBackend::set_location(const std::string &location) {
  if (location.size() > 100 || location.find_first_of("\r\n") != std::string::npos)
    return false;
  const bool saved = save_location(location);
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->view = {};
    impl_->view.location = location;
    impl_->view.status = location.empty() ? "Set city" : "Loading weather...";
    impl_->view.loading = !location.empty();
    impl_->pending = !location.empty();
    impl_->notify();
  }
  impl_->wake.notify_one();
  return saved;
}

}  // namespace cyberdeck
