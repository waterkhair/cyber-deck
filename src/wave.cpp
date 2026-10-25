#include "wave.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace cyberdeck {
namespace {

std::filesystem::path station_path() {
  const auto theme = theme_path();
  return theme.empty() ? std::filesystem::path{} : theme.parent_path() / "wave-stations.txt";
}

bool valid_station(const std::string &name, const std::string &url) {
  if (name.empty() || name.size() > 120 || url.size() > 2048) return false;
  if (std::all_of(name.begin(), name.end(), [](unsigned char ch) { return ch == ' '; }))
    return false;
  if (std::any_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
    return false;
  const size_t prefix = url.rfind("https://", 0) == 0 ? 8 :
                        url.rfind("http://", 0) == 0 ? 7 : 0;
  if (!prefix || url.size() <= prefix || url[prefix] == '/') return false;
  return std::none_of(url.begin(), url.end(), [](unsigned char ch) {
    return ch <= 32 || ch >= 127;
  });
}

#ifndef __ANDROID__
std::string mpris_script() {
  std::vector<std::filesystem::path> candidates = {
      "/etc/mpv/scripts/mpris.so", "/usr/lib/mpv-mpris/mpris.so"};
  if (const char *home = std::getenv("HOME"); home && *home)
    candidates.insert(candidates.begin() + 1,
                      std::filesystem::path(home) / ".config/mpv/scripts/mpris.so");
  for (const auto &path : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) return path.string();
  }
  return {};
}
#endif

}  // namespace

WaveBackend::WaveBackend() {
  for (const auto &[name, url] : kDefaultWaveStations)
    stations_.push_back({name, url});
  std::ifstream input(station_path());
  std::string name, url;
  while (stations_.size() < 128 && std::getline(input, name) && std::getline(input, url)) {
    if (valid_station(name, url) &&
        std::none_of(stations_.begin(), stations_.end(), [&](const WaveStation &station) {
          return station.url == url;
        }))
      stations_.push_back({name, url});
  }
}

WaveBackend::~WaveBackend() {
  terminate_active();
  (void)tick();
}

bool WaveBackend::save_stations(std::string &error) const {
  const auto path = station_path();
  if (path.empty()) { error = "HOME or XDG_CONFIG_HOME is required"; return false; }
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) { error = "Cannot create station directory"; return false; }
  const std::string pattern = path.string() + ".tmp.XXXXXX";
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back('\0');
  const int fd = ::mkstemp(temp.data());
  if (fd < 0) { error = "Cannot create station file: " + std::string(std::strerror(errno)); return false; }
  (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
  std::string contents;
  for (size_t i = kDefaultWaveStations.size(); i < stations_.size(); ++i)
    contents += stations_[i].name + '\n' + stations_[i].url + '\n';
  const char *data = contents.data();
  size_t remaining = contents.size();
  int failure = 0;
  while (remaining > 0) {
    const ssize_t size = ::write(fd, data, remaining);
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) { failure = size < 0 ? errno : EIO; break; }
    data += size;
    remaining -= static_cast<size_t>(size);
  }
  if (!failure && ::fsync(fd) < 0) failure = errno;
  if (::close(fd) < 0 && !failure) failure = errno;
  if (!failure && ::rename(temp.data(), path.c_str()) < 0) failure = errno;
  if (failure) {
    error = "Cannot save station file: " + std::string(std::strerror(failure));
    (void)::unlink(temp.data());
  }
  return failure == 0;
}

bool WaveBackend::add_station(const std::string &name, const std::string &url, std::string &error) {
  if (!valid_station(name, url)) {
    error = "Enter a name and a direct HTTP(S) stream URL";
    return false;
  }
  if (stations_.size() >= 128) { error = "Station limit reached"; return false; }
  if (std::any_of(stations_.begin(), stations_.end(), [&](const WaveStation &station) {
        return station.url == url;
      })) {
    error = "That stream URL is already listed";
    return false;
  }
  stations_.push_back({name, url});
  if (!save_stations(error)) {
    stations_.pop_back();
    return false;
  }
  return true;
}

void WaveBackend::terminate_active() {
  if (active_pid_ <= 0) return;
  (void)::kill(active_pid_, SIGTERM);
  retiring_.push_back(active_pid_);
  active_pid_ = -1;
}

bool WaveBackend::tick() {
  for (auto it = retiring_.begin(); it != retiring_.end();) {
    int code = 0;
    const pid_t result = ::waitpid(*it, &code, WNOHANG);
    if (result == *it || (result < 0 && errno == ECHILD)) it = retiring_.erase(it);
    else ++it;
  }
  if (active_pid_ <= 0) return false;
  int code = 0;
  const pid_t result = ::waitpid(active_pid_, &code, WNOHANG);
  if (result == 0 || (result < 0 && errno != ECHILD)) return false;
  active_pid_ = -1;
  view_.playing = kNoWaveStation;
#ifdef __ANDROID__
  view_.status = WIFEXITED(code) && WEXITSTATUS(code) == 0
                     ? "Opened in mpv-android; use Android media controls"
                     : "Android player launch failed; check app installation";
#else
  view_.status = WIFEXITED(code) && WEXITSTATUS(code) == 0 ? "Stream ended" :
                 WIFEXITED(code) ? "mpv exited with code " + std::to_string(WEXITSTATUS(code)) :
                                   "Playback stopped";
#endif
  return true;
}

void WaveBackend::stop() {
  terminate_active();
  view_.playing = kNoWaveStation;
#ifdef __ANDROID__
  view_.status = "Use Android media controls to stop playback";
#else
  view_.status = "Stopped";
#endif
}

void WaveBackend::toggle(size_t index) {
  if (index >= stations_.size()) return;
  if (active_pid_ > 0 && view_.playing == index) {
    stop();
    return;
  }
  terminate_active();
  view_.playing = kNoWaveStation;

  posix_spawn_file_actions_t actions;
  int error = ::posix_spawn_file_actions_init(&actions);
  if (error != 0) {
    view_.status = "Cannot prepare mpv: " + std::string(std::strerror(error));
    return;
  }
  error = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (error == 0) error = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  if (error == 0) error = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  if (error != 0) {
    ::posix_spawn_file_actions_destroy(&actions);
    view_.status = "Cannot prepare mpv: " + std::string(std::strerror(error));
    return;
  }
#ifdef __ANDROID__
  std::vector<char *> args = {
      const_cast<char *>("am"), const_cast<char *>("start"),
      const_cast<char *>("-a"), const_cast<char *>("android.intent.action.VIEW"),
      const_cast<char *>("-t"), const_cast<char *>("audio/*"),
      const_cast<char *>("-p"), const_cast<char *>("is.xyz.mpv"),
      const_cast<char *>("--es"), const_cast<char *>("title"),
      const_cast<char *>(stations_[index].name.c_str()),
      const_cast<char *>("-d"), const_cast<char *>(stations_[index].url.c_str()),
      nullptr};
  pid_t pid = -1;
  error = ::posix_spawnp(&pid, "am", &actions, nullptr, args.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (error != 0) {
    view_.status = "Cannot launch Android player: " + std::string(std::strerror(error));
    return;
  }
  active_pid_ = pid;
  view_.status = "Opening station in mpv-android...";
#else
  const std::string script = mpris_script();
  const std::string script_option = "--script=" + script;
  const std::string title_option = "--force-media-title=" + stations_[index].name;
  std::vector<char *> args = {
      const_cast<char *>("mpv"), const_cast<char *>("--no-video"),
      const_cast<char *>("--force-window=no"), const_cast<char *>("--terminal=no"),
      const_cast<char *>("--load-scripts=no"), const_cast<char *>("--scripts="),
      const_cast<char *>("--really-quiet")};
  if (!script.empty()) args.push_back(const_cast<char *>(script_option.c_str()));
  args.push_back(const_cast<char *>(title_option.c_str()));
  args.push_back(const_cast<char *>("--"));
  args.push_back(const_cast<char *>(stations_[index].url.c_str()));
  args.push_back(nullptr);
  pid_t pid = -1;
  error = ::posix_spawnp(&pid, "mpv", &actions, nullptr, args.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (error != 0) {
    view_.status = error == ENOENT ? "mpv is not installed" :
                   "Cannot start mpv: " + std::string(std::strerror(error));
    return;
  }
  active_pid_ = pid;
  view_.playing = index;
  view_.status = script.empty() ? "mpv running; no Linux media status (mpv-mpris missing)"
                                : "mpv running";
#endif
}

}  // namespace cyberdeck
