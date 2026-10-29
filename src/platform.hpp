#pragma once

#include <vterm.h>

#include <curses.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <climits>
#include <clocale>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace cyberdeck {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr size_t kOutputBatch = 32 * 1024;
constexpr size_t kQueueLimit = 64 * 1024;
constexpr size_t kHistoryBytes = 2 * 1024 * 1024;
constexpr size_t kHistoryLines = 500;
constexpr size_t kMaxSessions = 4;
constexpr int kMaxRows = 200;
constexpr int kMaxCols = 400;

struct UiLayout {
  int inset = 1;
  int terminal_top() const { return inset + 4; }  // Three-row menu and one spacer row.
  int reserved_rows() const { return 2 * inset + 5; }  // Menu, spacer, footer, margins.
};

inline UiLayout ui_layout() {
  const char *term = std::getenv("TERM");
  // Ghostty already supplies pixel padding outside the character grid.
  return {term && std::strcmp(term, "xterm-ghostty") == 0 ? 0 : 1};
}
constexpr auto kFrameInterval = 33ms;
constexpr auto kAltDelay = 40ms;

inline int signal_write_fd = -1;
inline volatile sig_atomic_t signal_bits = 0;
constexpr int kResizeSignal = 1;
constexpr int kChildSignal = 2;
constexpr int kQuitSignal = 4;

inline void signal_handler(int signo) {
  if (signo == SIGWINCH) signal_bits |= kResizeSignal;
  if (signo == SIGCHLD) signal_bits |= kChildSignal;
  if (signo == SIGTERM || signo == SIGHUP || signo == SIGINT) signal_bits |= kQuitSignal;
  const char byte = 1;
  if (signal_write_fd >= 0) {
    const int saved = errno;
    (void)::write(signal_write_fd, &byte, 1);
    errno = saved;
  }
}

inline void close_fd(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

inline void nonblocking_cloexec(int fd) {
  if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0 ||
      fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC) < 0) {
    throw std::runtime_error("cannot configure file descriptor");
  }
}

inline std::string select_shell(int argc, char **argv) {
  std::string chosen;
  bool explicit_shell = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--shell") == 0 && i + 1 < argc) {
      chosen = argv[++i];
      explicit_shell = true;
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::cout << "usage: cyber-deck [--shell /absolute/path]\n";
      return {};
    } else {
      throw std::runtime_error("usage: cyber-deck [--shell /absolute/path]");
    }
  }
  auto valid = [](const std::string &path) {
    struct stat info {};
    return !path.empty() && path[0] == '/' && ::stat(path.c_str(), &info) == 0 &&
           S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
  };
  if (explicit_shell) {
    if (!valid(chosen)) throw std::runtime_error("--shell requires an executable absolute path");
    return chosen;
  }
  const char *environment_shell = std::getenv("SHELL");
  if (environment_shell && valid(environment_shell)) return environment_shell;
  if (passwd *account = getpwuid(getuid()); account && account->pw_shell && valid(account->pw_shell))
    return account->pw_shell;
  if (valid("/bin/sh")) return "/bin/sh";
  throw std::runtime_error("no usable shell found");
}

class CursesHost {
 public:
  CursesHost() {
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO))
      throw std::runtime_error("interactive terminal required on stdin and stdout");
    if (::tcgetattr(STDIN_FILENO, &original_) < 0)
      throw std::runtime_error("cannot read host terminal settings");
    have_original_ = true;
    if (!initscr()) throw std::runtime_error("ncurses initialization failed");
    active_ = true;
    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    set_escdelay(25);
    (void)curs_set(1);
    if (has_colors()) {
      start_color();
      default_colors_ = use_default_colors() == OK;
    }
    clear();
  }

  ~CursesHost() {
    if (active_) {
      curs_set(1);
      endwin();
    }
    if (have_original_) (void)::tcsetattr(STDIN_FILENO, TCSANOW, &original_);
  }

  CursesHost(const CursesHost &) = delete;
  CursesHost &operator=(const CursesHost &) = delete;
  bool default_colors() const { return default_colors_; }

 private:
  termios original_{};
  bool have_original_ = false;
  bool active_ = false;
  bool default_colors_ = false;
};

class Signals {
 public:
  Signals() {
    if (::pipe(pipe_) < 0) throw std::runtime_error("cannot create signal pipe");
    try {
      nonblocking_cloexec(pipe_[0]);
      nonblocking_cloexec(pipe_[1]);
      signal_write_fd = pipe_[1];
      struct sigaction action {};
      action.sa_handler = signal_handler;
      sigemptyset(&action.sa_mask);
      action.sa_flags = 0;
      for (size_t i = 0; i < watched_.size(); ++i) {
        if (sigaction(watched_[i], &action, &old_[i]) < 0)
          throw std::runtime_error("cannot install signal handler");
        installed_ = i + 1;
      }
    } catch (...) {
      restore();
      throw;
    }
  }

  ~Signals() { restore(); }
  Signals(const Signals &) = delete;
  Signals &operator=(const Signals &) = delete;
  int fd() const { return pipe_[0]; }

  int consume() {
    sigset_t blocked{};
    sigset_t previous{};
    sigemptyset(&blocked);
    for (int signo : watched_) sigaddset(&blocked, signo);
    (void)sigprocmask(SIG_BLOCK, &blocked, &previous);
    std::array<char, 64> buffer{};
    while (::read(pipe_[0], buffer.data(), buffer.size()) > 0) {}
    int result = signal_bits;
    signal_bits = 0;
    (void)sigprocmask(SIG_SETMASK, &previous, nullptr);
    return result;
  }

 private:
  void restore() {
    signal_write_fd = -1;
    for (size_t i = 0; i < installed_; ++i) (void)sigaction(watched_[i], &old_[i], nullptr);
    close_fd(pipe_[0]);
    close_fd(pipe_[1]);
  }
  int pipe_[2] = {-1, -1};
  std::array<int, 5> watched_ = {SIGWINCH, SIGCHLD, SIGTERM, SIGHUP, SIGINT};
  std::array<struct sigaction, 5> old_{};
  size_t installed_ = 0;
};

}  // namespace cyberdeck
