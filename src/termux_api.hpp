#pragma once

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <json-c/json.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <signal.h>

extern char **environ;

namespace cyberdeck::termux {

using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

inline Json parse(const std::string &value) {
  return Json(json_tokener_parse(value.c_str()), json_object_put);
}

inline json_object *field(json_object *object, const char *name) {
  json_object *value = nullptr;
  return object && json_object_is_type(object, json_type_object) &&
         json_object_object_get_ex(object, name, &value) ? value : nullptr;
}

inline std::string string(json_object *object, const char *name) {
  json_object *value = field(object, name);
  return value && json_object_is_type(value, json_type_string) ? json_object_get_string(value) : "";
}

inline int integer(json_object *object, const char *name, int fallback = 0) {
  json_object *value = field(object, name);
  return value && json_object_is_type(value, json_type_int) ? json_object_get_int(value) : fallback;
}

inline bool boolean(json_object *object, const char *name) {
  json_object *value = field(object, name);
  return value && json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
}

struct Result {
  std::string output;
  std::string error;
  bool ok = false;
};

inline Result run(const std::vector<std::string> &command) {
  if (command.empty()) return {{}, "No command", false};
  int fds[2];
  if (::pipe(fds) != 0) return {{}, "Cannot create API pipe", false};
  (void)::fcntl(fds[0], F_SETFL, O_NONBLOCK);
  posix_spawn_file_actions_t actions;
  int error = ::posix_spawn_file_actions_init(&actions);
  const bool actions_ready = error == 0;
  if (error == 0) error = ::posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  if (error == 0) error = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                                               "/dev/null", O_WRONLY, 0);
  if (error == 0) error = ::posix_spawn_file_actions_addclose(&actions, fds[0]);
  if (error == 0) error = ::posix_spawn_file_actions_addclose(&actions, fds[1]);
  std::vector<char *> args;
  for (const auto &item : command) args.push_back(const_cast<char *>(item.c_str()));
  args.push_back(nullptr);
  pid_t pid = -1;
  if (error == 0) error = ::posix_spawnp(&pid, command[0].c_str(), &actions,
                                         nullptr, args.data(), environ);
  if (actions_ready) ::posix_spawn_file_actions_destroy(&actions);
  ::close(fds[1]);
  if (error != 0) {
    ::close(fds[0]);
    return {{}, error == ENOENT ? "Install the termux-api package" : "Cannot start Termux:API", false};
  }

  Result result;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool done = false, eof = false;
  int status = 0;
  while (!done || !eof) {
    char buffer[4096];
    const ssize_t size = ::read(fds[0], buffer, sizeof(buffer));
    if (size > 0) {
      if (result.output.size() + static_cast<size_t>(size) > 131072) {
        result.error = "Termux:API response too large";
        break;
      }
      result.output.append(buffer, static_cast<size_t>(size));
    } else if (size == 0) eof = true;
    else if (errno != EAGAIN && errno != EINTR) { result.error = "Cannot read Termux:API"; break; }
    if (!done) {
      const pid_t waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid || (waited < 0 && errno == ECHILD)) done = true;
    }
    if (done && eof) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      result.error = "Termux:API timed out; check add-on and permissions";
      break;
    }
    pollfd descriptor{fds[0], POLLIN, 0};
    (void)::poll(&descriptor, 1, 100);
  }
  ::close(fds[0]);
  if (!done) {
    (void)::kill(pid, SIGKILL);
    (void)::waitpid(pid, &status, 0);
  }
  result.ok = result.error.empty() && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!result.ok && result.error.empty()) result.error = "Termux:API call failed; check its Android add-on";
  return result;
}

template <typename Snapshot>
class Worker {
 public:
  explicit Worker(Snapshot initial) : state_(std::move(initial)) {
    if (::pipe(pipe_) != 0) throw std::runtime_error("Cannot create Termux notification pipe");
    (void)::fcntl(pipe_[0], F_SETFL, O_NONBLOCK);
    (void)::fcntl(pipe_[1], F_SETFL, O_NONBLOCK);
    thread_ = std::thread([this] { loop(); });
  }

  ~Worker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      tasks_.clear();
    }
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
    ::close(pipe_[0]);
    ::close(pipe_[1]);
  }

  Worker(const Worker &) = delete;
  Worker &operator=(const Worker &) = delete;

  int fd() const { return pipe_[0]; }

  Snapshot snapshot() {
    char bytes[64];
    while (::read(pipe_[0], bytes, sizeof(bytes)) > 0) {}
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  void post(std::function<void(Snapshot &)> task,
            std::function<void(Snapshot &)> pending = {}) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.clear();
      tasks_.push_back(std::move(task));
      if (pending) {
        pending(state_);
        const char byte = 1;
        (void)::write(pipe_[1], &byte, 1);
      }
    }
    ready_.notify_one();
  }

 private:
  void loop() {
    for (;;) {
      std::function<void(Snapshot &)> task;
      Snapshot next;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (stop_) return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
        next = state_;
      }
      task(next);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        state_ = std::move(next);
      }
      const char byte = 1;
      (void)::write(pipe_[1], &byte, 1);
    }
  }

  int pipe_[2] = {-1, -1};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::function<void(Snapshot &)>> tasks_;
  Snapshot state_;
  bool stop_ = false;
};

}  // namespace cyberdeck::termux
