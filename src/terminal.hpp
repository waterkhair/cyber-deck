#pragma once

#include "platform.hpp"

extern char **environ;

namespace cyberdeck {

class PtySession {
 public:
  ~PtySession() {
    if (child_ > 0 && !reaped_) {
      signal_groups(SIGKILL);
      int status = 0;
      while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
    }
    close_fd(master_);
  }

  PtySession(const PtySession &) = delete;
  PtySession &operator=(const PtySession &) = delete;
  PtySession() = default;

  void start(const std::string &program, int rows, int cols,
             const std::vector<std::string> &arguments = {}) {
    if (child_ > 0 && !reaped_) throw std::runtime_error("previous shell still running");
    close_fd(master_);
    queue_.clear();
    queue_offset_ = 0;
    reaped_ = false;
    status_ = 0;
    error_.clear();
    stopping_ = false;
    stage_ = 0;

    termios slave{};
    if (tcgetattr(STDIN_FILENO, &slave) < 0)
      throw std::runtime_error("cannot read terminal settings for PTY");
    cfmakeraw(&slave);
    slave.c_lflag |= ICANON | ECHO | ISIG | IEXTEN;
    slave.c_iflag |= ICRNL | IXON;
    slave.c_oflag |= OPOST | ONLCR;
    slave.c_cc[VINTR] = 3;
    slave.c_cc[VSUSP] = 26;
    slave.c_cc[VERASE] = 127;

    winsize size{};
    size.ws_row = static_cast<unsigned short>(rows);
    size.ws_col = static_cast<unsigned short>(cols);
    const char *name = std::strrchr(program.c_str(), '/');
    name = name ? name + 1 : program.c_str();
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(name));
    if (arguments.empty()) argv.push_back(const_cast<char *>("-i"));
    else for (const auto &argument : arguments)
      argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char **entry = ::environ; entry && *entry; ++entry)
      if (std::strncmp(*entry, "TERM=", 5) != 0) environment.emplace_back(*entry);
    environment.emplace_back("TERM=xterm");
    std::vector<char *> envp;
    for (auto &entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    pid_t child = forkpty(&master_, nullptr, &slave, &size);
    if (child < 0) throw std::runtime_error("forkpty failed");
    if (child == 0) {
      // Prepare argv and environment before fork; only execve runs in the child.
      execve(program.c_str(), argv.data(), envp.data());
      _exit(127);
    }
    child_ = child;
    try {
      nonblocking_cloexec(master_);
    } catch (...) {
      signal_groups(SIGKILL);
      int status = 0;
      while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
      reaped_ = true;
      close_fd(master_);
      throw;
    }
  }

  int fd() const { return master_; }
  bool active() const { return child_ > 0 && !reaped_; }
  bool stopping() const { return stopping_; }
  bool writable() const { return master_ >= 0 && queue_offset_ < queue_.size(); }
  size_t free_queue() const { return kQueueLimit - (queue_.size() - queue_offset_); }
  const std::string &error() const { return error_; }

  bool enqueue(const char *data, size_t size) {
    if (master_ < 0 || size > free_queue()) return false;
    if (queue_offset_ && queue_offset_ >= queue_.size() / 2) {
      queue_.erase(0, queue_offset_);
      queue_offset_ = 0;
    }
    queue_.append(data, size);
    return true;
  }

  void flush(size_t budget = kOutputBatch) {
    size_t written = 0;
    while (writable() && written < budget) {
      size_t amount = std::min(budget - written, queue_.size() - queue_offset_);
      ssize_t count = ::write(master_, queue_.data() + queue_offset_, amount);
      if (count > 0) {
        queue_offset_ += static_cast<size_t>(count);
        written += static_cast<size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        close_fd(master_);
        queue_.clear();
        queue_offset_ = 0;
        break;
      }
    }
    if (queue_offset_ == queue_.size()) {
      queue_.clear();
      queue_offset_ = 0;
    }
  }

  template <typename Consumer>
  void read_output(Consumer consume, size_t budget = kOutputBatch) {
    if (master_ < 0) return;
    std::array<char, 4096> buffer{};
    size_t total = 0;
    while (total < budget) {
      ssize_t count = ::read(master_, buffer.data(), std::min(buffer.size(), budget - total));
      if (count > 0) {
        total += static_cast<size_t>(count);
        consume(buffer.data(), static_cast<size_t>(count));
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        close_fd(master_);
        queue_.clear();
        queue_offset_ = 0;
        break;
      }
    }
  }

  void resize(int rows, int cols) {
    if (master_ < 0) return;
    winsize size{};
    size.ws_row = static_cast<unsigned short>(rows);
    size.ws_col = static_cast<unsigned short>(cols);
    (void)ioctl(master_, TIOCSWINSZ, &size);
  }

  void close_master() {
    close_fd(master_);
    queue_.clear();
    queue_offset_ = 0;
  }

  void reap() {
    if (!active()) return;
    int status = 0;
    pid_t result = waitpid(child_, &status, WNOHANG);
    if (result == child_) {
      reaped_ = true;
      status_ = status;
    } else if (result < 0 && errno == ECHILD) {
      reaped_ = true;
      error_ = "shell could not be reaped";
    }
  }

  std::string state() const {
    if (!error_.empty()) return error_;
    if (active()) return stopping_ ? "closing" : "shell running";
    if (child_ == 0) return "shell not started";
    if (WIFEXITED(status_)) return "shell exited " + std::to_string(WEXITSTATUS(status_));
    if (WIFSIGNALED(status_)) return "shell signal " + std::to_string(WTERMSIG(status_));
    return "shell ended";
  }

  void begin_shutdown() {
    if (!active() || stopping_) return;
    stopping_ = true;
    stage_ = 0;
    signal_groups(SIGHUP);
    deadline_ = Clock::now() + 250ms;
  }

  void advance_shutdown() {
    if (!stopping_ || !active() || Clock::now() < deadline_) return;
    if (stage_ == 0) {
      signal_groups(SIGTERM);
      stage_ = 1;
      deadline_ = Clock::now() + 250ms;
    } else if (stage_ == 1) {
      signal_groups(SIGKILL);
      stage_ = 2;
      deadline_ = Clock::now() + 250ms;
    } else {
      // The child normally reaps immediately after SIGKILL; retry without spin.
      signal_groups(SIGKILL);
      deadline_ = Clock::now() + 250ms;
    }
  }

  Clock::time_point shutdown_deadline() const { return deadline_; }

 private:
  void signal_groups(int signo) {
    if (child_ <= 0 || reaped_) return;
    if (master_ >= 0) {
      pid_t foreground = -1;
      if (ioctl(master_, TIOCGPGRP, &foreground) == 0 && foreground > 0 &&
          foreground != child_ && getsid(foreground) == child_)
        (void)::kill(-foreground, signo);
    }
    // forkpty creates a new session whose ID is the shell PID.
    (void)::kill(-child_, signo);
  }

  int master_ = -1;
  pid_t child_ = 0;
  bool reaped_ = false;
  int status_ = 0;
  std::string error_;
  std::string queue_;
  size_t queue_offset_ = 0;
  bool stopping_ = false;
  int stage_ = 0;
  Clock::time_point deadline_{};
};

class TerminalModel {
 public:
  TerminalModel(PtySession &session, int rows, int cols) : session_(session), rows_(rows), cols_(cols) {
    VTermBuilder builder{};
    builder.rows = rows;
    builder.cols = cols;
    builder.outbuffer_len = kQueueLimit;
    term_ = vterm_build(&builder);
    if (!term_) throw std::runtime_error("libvterm initialization failed");
    vterm_set_utf8(term_, 1);
    screen_ = vterm_obtain_screen(term_);
    vterm_screen_enable_altscreen(screen_, 1);
    callbacks_.damage = [](VTermRect, void *user) -> int { static_cast<TerminalModel *>(user)->dirty_ = true; return 1; };
    callbacks_.moverect = [](VTermRect, VTermRect, void *user) -> int { static_cast<TerminalModel *>(user)->dirty_ = true; return 1; };
    callbacks_.movecursor = [](VTermPos, VTermPos, int visible, void *user) -> int {
      auto *self = static_cast<TerminalModel *>(user);
      self->cursor_visible_ = visible != 0;
      self->dirty_ = true;
      return 1;
    };
    callbacks_.resize = [](int, int, void *user) -> int { static_cast<TerminalModel *>(user)->dirty_ = true; return 1; };
    callbacks_.sb_pushline = [](int count, const VTermScreenCell *cells, void *user) -> int {
      return static_cast<TerminalModel *>(user)->push_history(count, cells);
    };
    callbacks_.sb_popline = [](int count, VTermScreenCell *cells, void *user) -> int {
      return static_cast<TerminalModel *>(user)->pop_history(count, cells);
    };
    callbacks_.sb_clear = [](void *user) -> int {
      auto *self = static_cast<TerminalModel *>(user);
      self->history_.clear(); self->history_bytes_ = 0; return 1;
    };
    vterm_screen_set_callbacks(screen_, &callbacks_, this);
    vterm_screen_reset(screen_, 1);
    vterm_output_set_callback(term_, [](const char *data, size_t size, void *user) {
      auto *self = static_cast<TerminalModel *>(user);
      if (!self->session_.enqueue(data, size)) self->overflow_ = true;
    }, this);
  }

  ~TerminalModel() { if (term_) vterm_free(term_); }
  TerminalModel(const TerminalModel &) = delete;
  TerminalModel &operator=(const TerminalModel &) = delete;

  void feed(const char *data, size_t size) {
    (void)vterm_input_write(term_, data, size);
    vterm_screen_flush_damage(screen_);
  }
  void key(VTermKey key, VTermModifier modifiers = VTERM_MOD_NONE) {
    vterm_keyboard_key(term_, key, modifiers);
  }
  void character(uint32_t value) { vterm_keyboard_unichar(term_, value, VTERM_MOD_NONE); }
  void resize(int rows, int cols) {
    if (rows == rows_ && cols == cols_) return;
    rows_ = rows; cols_ = cols;
    vterm_set_size(term_, rows, cols);
    session_.resize(rows, cols);
    dirty_ = true;
  }
  int rows() const { return rows_; }
  int cols() const { return cols_; }
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }
  bool overflow() const { return overflow_; }
  bool cursor_visible() const { return cursor_visible_; }
  VTermScreen *screen() const { return screen_; }
  VTermState *state() const { return vterm_obtain_state(term_); }

 private:
  int push_history(int count, const VTermScreenCell *cells) {
    if (count <= 0 || count > kMaxCols) return 0;
    const size_t bytes = static_cast<size_t>(count) * sizeof(VTermScreenCell);
    if (bytes > kHistoryBytes) return 0;
    history_.emplace_back(cells, cells + count);
    history_bytes_ += bytes;
    while (history_.size() > kHistoryLines || history_bytes_ > kHistoryBytes) {
      history_bytes_ -= history_.front().size() * sizeof(VTermScreenCell);
      history_.pop_front();
    }
    return 1;
  }

  int pop_history(int count, VTermScreenCell *cells) {
    if (count <= 0 || history_.empty()) return 0;
    auto line = std::move(history_.back());
    history_.pop_back();
    history_bytes_ -= line.size() * sizeof(VTermScreenCell);
    size_t copy = std::min(static_cast<size_t>(count), line.size());
    std::copy_n(line.data(), copy, cells);
    for (size_t i = copy; i < static_cast<size_t>(count); ++i) cells[i] = VTermScreenCell{};
    return 1;
  }

  PtySession &session_;
  VTerm *term_ = nullptr;
  VTermScreen *screen_ = nullptr;
  VTermScreenCallbacks callbacks_{}; // libvterm retains this pointer.
  int rows_, cols_;
  bool dirty_ = true;
  bool overflow_ = false;
  bool cursor_visible_ = true;
  std::deque<std::vector<VTermScreenCell>> history_;
  size_t history_bytes_ = 0;
};

struct TerminalSlot {
  explicit TerminalSlot(const std::string &shell, int rows, int cols) {
    model = std::make_unique<TerminalModel>(session, rows, cols);
    session.start(shell, rows, cols);
  }

  TerminalSlot(const std::string &program, const std::vector<std::string> &arguments,
               int rows, int cols) {
    model = std::make_unique<TerminalModel>(session, rows, cols);
    session.start(program, rows, cols, arguments);
  }

  PtySession session;
  std::unique_ptr<TerminalModel> model;
  bool closing = false;
  bool error_shutdown = false;
};

}  // namespace cyberdeck
