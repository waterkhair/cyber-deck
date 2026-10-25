#pragma once

#include "dashboard.hpp"
#include "elinks.hpp"
#include "input.hpp"
#include "notes.hpp"

#include <cstdio>
#include <fcntl.h>

namespace cyberdeck {

class Application {
 public:
  Application(const std::string &shell, bool default_colors)
      : shell_(shell), renderer_(default_colors),
        debug_input_(std::getenv("CYBER_DECK_DEBUG_INPUT") != nullptr) {
    theme_ = load_theme();
    theme_selection_ = static_cast<size_t>(theme_);
    cookie_policy_ = load_cookie_policy();
    cookie_selection_ = static_cast<size_t>(cookie_policy_);
    show_web_header_links_ = load_web_header_links();
    renderer_.set_theme(theme_);
    create_slot(0);
    web_tabs_[0] = std::make_unique<WebTab>();
    web_order_.push_back(0);
  }

  void run() {
    while (!done_) {
      int signals = signals_.consume();
      if (signals & kQuitSignal) request_quit(true);
      if (signals & kResizeSignal) resize();
      if (wave_.tick()) ui_dirty_ = true;

      size_t keys = 0;
      while (keys < 64) {
        wint_t code = 0;
        int result = wget_wch(stdscr, &code);
        if (result == ERR) break;
        ++keys;
        handle_key({code, result == KEY_CODE_YES});
      }
      const bool input_pending = keys == 64;
      flush_escape_if_due();

      if (wifi_.fd() >= 0) {
        pollfd update{wifi_.fd(), POLLIN, 0};
        if (::poll(&update, 1, 0) > 0 && (update.revents & POLLIN)) {
          WifiNetwork previous;
          const bool had_selection = wifi_selection_ < wifi_view_.networks.size();
          if (had_selection) previous = wifi_view_.networks[wifi_selection_];
          wifi_view_ = wifi_.snapshot();
          if (wifi_forgetting_request_ && wifi_view_.forget_epoch != wifi_forget_start_epoch_)
            wifi_forgetting_request_ = false;
          if (!wifi_view_.networks.empty()) {
            wifi_selection_ = std::min(wifi_selection_, wifi_view_.networks.size() - 1);
            if (had_selection) {
              auto selected = std::find_if(wifi_view_.networks.begin(), wifi_view_.networks.end(),
                  [&previous](const WifiNetwork &network) {
                    if (!previous.path.empty() && network.path == previous.path) return true;
                    if (!previous.saved_uuid.empty() && network.saved_uuid == previous.saved_uuid) return true;
                    return network.ssid == previous.ssid && network.visible == previous.visible;
                  });
              if (selected != wifi_view_.networks.end())
                wifi_selection_ = static_cast<size_t>(selected - wifi_view_.networks.begin());
            }
          } else wifi_selection_ = 0;
          ui_dirty_ = true;
        }
      }
      if (audio_.fd() >= 0) {
        pollfd update{audio_.fd(), POLLIN, 0};
        if (::poll(&update, 1, 0) > 0 && (update.revents & POLLIN)) {
          const bool output = audio_selection_ == 0;
          const auto &old_devices = output ? audio_view_.outputs : audio_view_.inputs;
          const std::string old_name = audio_picker_ && audio_picker_selection_ < old_devices.size() ?
              old_devices[audio_picker_selection_].name : "";
          audio_view_ = audio_.snapshot();
          const auto &new_devices = output ? audio_view_.outputs : audio_view_.inputs;
          auto found = std::find_if(new_devices.begin(), new_devices.end(),
              [&](const AudioDevice &device) { return device.name == old_name; });
          if (!old_name.empty() && found != new_devices.end())
            audio_picker_selection_ = static_cast<size_t>(found - new_devices.begin());
          else
            audio_picker_selection_ = new_devices.empty() ? 0 :
                std::min(audio_picker_selection_, new_devices.size() - 1);
          ui_dirty_ = true;
        }
      }
      if (bluetooth_.fd() >= 0) {
        pollfd update{bluetooth_.fd(), POLLIN, 0};
        if (::poll(&update, 1, 0) > 0 && (update.revents & POLLIN)) {
          const std::string old_path = bluetooth_selection_ < bluetooth_view_.devices.size() ?
              bluetooth_view_.devices[bluetooth_selection_].path : "";
          const uint64_t old_prompt = bluetooth_view_.prompt_epoch;
          bluetooth_view_ = bluetooth_.snapshot();
          if (bluetooth_view_.prompt_epoch != old_prompt ||
              bluetooth_view_.prompt == BluetoothPromptKind::None) {
            bluetooth_code_.clear();
            bluetooth_answered_epoch_ = 0;
            modal_accept_selected_ = bluetooth_view_.prompt != BluetoothPromptKind::Confirm;
          }
          auto found = std::find_if(bluetooth_view_.devices.begin(), bluetooth_view_.devices.end(),
              [&](const BluetoothDevice &device) { return device.path == old_path; });
          if (!old_path.empty() && found != bluetooth_view_.devices.end())
            bluetooth_selection_ = static_cast<size_t>(found - bluetooth_view_.devices.begin());
          else bluetooth_selection_ = bluetooth_view_.devices.empty() ? 0 :
              std::min(bluetooth_selection_, bluetooth_view_.devices.size() - 1);
          ui_dirty_ = true;
        }
      }
      // Each slot gets a bounded turn, including hidden slots. Only the selected
      // model can trigger a frame, so hidden output costs no screen rendering.
      for (size_t i = 0; i < kMaxSessions; ++i) {
        auto &slot = slots_[i];
        if (!slot) continue;
        slot->session.reap();
        slot->session.advance_shutdown();
        if (slot->session.fd() >= 0) {
          slot->session.read_output([&slot](const char *data, size_t size) {
            slot->model->feed(data, size);
          }, kOutputBatch / kMaxSessions);
          if (slot->session.writable()) slot->session.flush(kOutputBatch / kMaxSessions);
        }
        if (slot->model->overflow() && !slot->error_shutdown) {
          notice_ = "terminal " + std::to_string(i + 1) + " reply queue exceeded limit";
          slot->error_shutdown = true;
          slot->closing = true;
          slot->session.begin_shutdown();
          ui_dirty_ = true;
        }
        if (!slot->session.active()) {
          // A shell that exits leaves no interactive terminal behind. Read its
          // final output above, then remove the tab and select a live one.
          const bool exited_normally = !slot->closing && !slot->error_shutdown;
          slot->session.close_master();
          slot.reset();
          if (active_index_ == i) select_existing_slot();
          if (!quitting_ && exited_normally)
            notice_ = "Terminal " + std::to_string(i + 1) + " exited";
          ui_dirty_ = true;
        }
      }
      if (note_editor_) {
        auto &editor = *note_editor_;
        editor.session.reap();
        editor.session.advance_shutdown();
        if (editor.session.fd() >= 0) {
          editor.session.read_output([&editor](const char *data, size_t size) {
            editor.model->feed(data, size);
          }, kOutputBatch / kMaxSessions);
          if (editor.session.writable()) editor.session.flush(kOutputBatch / kMaxSessions);
        }
        if (editor.model->overflow() && !editor.error_shutdown) {
          notice_ = "Notes editor reply queue exceeded limit";
          editor.error_shutdown = true;
          editor.session.begin_shutdown();
          ui_dirty_ = true;
        }
        if (!editor.session.active() && editor.session.fd() < 0) {
          note_editor_.reset();
          refresh_notes();
          ui_dirty_ = true;
        }
      }
      for (auto &tab : web_tabs_) {
        if (!tab || !tab->browser) continue;
        auto &browser = *tab->browser;
        browser.session.reap();
        browser.session.advance_shutdown();
        if (browser.session.fd() >= 0) {
          browser.session.read_output([&browser](const char *data, size_t size) {
            browser.model->feed(data, size);
          }, kOutputBatch / kMaxSessions);
          if (browser.session.writable()) browser.session.flush(kOutputBatch / kMaxSessions);
        }
        if (browser.model->overflow() && !browser.error_shutdown) {
          notice_ = "Web browser reply queue exceeded limit";
          browser.error_shutdown = true;
          browser.session.begin_shutdown();
          ui_dirty_ = true;
        }
        if (!browser.session.active() &&
            (tab->browser_closing || browser.session.fd() < 0)) {
          browser.session.close_master();
          tab->browser.reset();
          tab->browser_closing = false;
          ui_dirty_ = true;
        }
      }
      if (quitting_ && !any_slots() && !note_editor_ && !any_web_browser()) done_ = true;

      auto now = Clock::now();
      if (active_tab_ == 0 && now >= next_overview_) {
        overview_ = read_system_overview();
        next_overview_ = now + 5s;
        ui_dirty_ = true;
      }
      const std::time_t clock_minute = std::time(nullptr) / 60;
      if (clock_minute != last_drawn_minute_) ui_dirty_ = true;
      TerminalSlot *selected = !menu_focused_ && active_tab_ == 1 ? active_slot() :
                               !menu_focused_ && active_tab_ == 3 && notes_section_ == 0 ? note_editor_.get() :
                               !menu_focused_ && active_tab_ == 4 &&
                                   !web_tab().browser_closing ? web_tab().browser.get() : nullptr;
      ModalView current_modal = modal();
      const bool selected_dirty = selected && selected->model->dirty() &&
                                  current_modal.kind == ModalView::Kind::None;
      std::string status_text = status();
      bool status_changed = status_text != last_drawn_status_;
      if (selected_dirty || status_changed || ui_dirty_) {
        if (now >= next_frame_ || ui_dirty_) {
          ChromeView view;
          view.tab = active_tab_;
          view.menu_selection = menu_selection_;
          view.menu_focused = menu_focused_;
          const bool bluetooth_prompt_active = bluetooth_view_.prompt != BluetoothPromptKind::None &&
              bluetooth_answered_epoch_ != bluetooth_view_.prompt_epoch;
          if (confirm_ != Confirm::None || wifi_forget_prompt_ || bluetooth_forget_prompt_ ||
              (bluetooth_prompt_active && bluetooth_view_.prompt == BluetoothPromptKind::Confirm))
            view.help_dialog = HelpDialog::Confirm;
          else if (wifi_password_prompt_ || bluetooth_prompt_active)
            view.help_dialog = HelpDialog::Password;
          else if (wave_add_stage_ != WaveAddStage::None || note_name_prompt_ || web_query_prompt_)
            view.help_dialog = HelpDialog::TextEntry;
          view.theme = theme_;
          view.settings_selection = settings_selection_;
          view.theme_selection = theme_selection_;
          view.cookie_policy = cookie_policy_;
          view.cookie_selection = cookie_selection_;
          view.show_web_header_links = show_web_header_links_;
          view.wifi_page = wifi_page_;
          view.audio_page = audio_page_;
          view.wifi_selection = wifi_selection_;
          view.wifi = wifi_view_;
          view.audio_selection = audio_selection_;
          view.audio_picker_selection = audio_picker_selection_;
          view.audio_picker = audio_picker_;
          view.audio = audio_view_;
          view.bluetooth_page = bluetooth_page_;
          view.bluetooth_selection = bluetooth_selection_;
          view.bluetooth = bluetooth_view_;
          view.notes_section = notes_section_;
          view.web_tab = web_position();
          for (size_t i = 0; i < web_order_.size(); ++i)
            view.web_tabs[i] = true;
          view.wave_selection = wave_selection_;
          view.wave = wave_.snapshot();
          view.wave_stations = wave_.stations();
          view.notes = notes_;
          view.note_selection = note_selection_;
          view.note_editing = note_editor_ != nullptr;
          view.web_browsing = web_tab().browser != nullptr && !web_tab().browser_closing;
          if (wifi_forgetting_request_) {
            view.wifi.forgetting = true;
            view.wifi.status = "Forgetting saved profiles...";
          }
          view.overview = overview_;
          view.active_terminal = active_index_;
          for (size_t i = 0; i < kMaxSessions; ++i)
            view.terminals[i] = slots_[i] != nullptr;
          renderer_.draw(selected ? selected->model.get() : nullptr,
                         view, status_text, current_modal);
          last_drawn_status_ = std::move(status_text);
          last_drawn_minute_ = clock_minute;
          next_frame_ = now + kFrameInterval;
          ui_dirty_ = false;
        }
      }

      if (done_) break;
      if (input_pending) continue;
      std::array<pollfd, 16 + 2 * kMaxSessions> descriptors{};
      nfds_t count = 0;
      descriptors[count++] = {STDIN_FILENO, POLLIN, 0};
      descriptors[count++] = {signals_.fd(), POLLIN, 0};
      if (wifi_.fd() >= 0) descriptors[count++] = {wifi_.fd(), POLLIN, 0};
      if (audio_.fd() >= 0) descriptors[count++] = {audio_.fd(), POLLIN, 0};
      if (bluetooth_.fd() >= 0) descriptors[count++] = {bluetooth_.fd(), POLLIN, 0};
      for (const auto &slot : slots_) {
        if (slot && slot->session.fd() >= 0)
          descriptors[count++] = {slot->session.fd(),
              static_cast<short>(POLLIN | (slot->session.writable() ? POLLOUT : 0)), 0};
      }
      if (note_editor_ && note_editor_->session.fd() >= 0)
        descriptors[count++] = {note_editor_->session.fd(),
            static_cast<short>(POLLIN | (note_editor_->session.writable() ? POLLOUT : 0)), 0};
      for (auto &tab : web_tabs_)
        if (tab && tab->browser && tab->browser->session.fd() >= 0)
          descriptors[count++] = {tab->browser->session.fd(),
              static_cast<short>(POLLIN | (tab->browser->session.writable() ? POLLOUT : 0)), 0};
      int timeout = -1;
      now = Clock::now();
      if (selected_dirty || ui_dirty_ || status_changed)
        timeout = milliseconds_until(next_frame_, now);
      const int clock_wait = static_cast<int>((60 - std::time(nullptr) % 60) * 1000);
      timeout = timeout < 0 ? clock_wait : std::min(timeout, clock_wait);
      if (active_tab_ == 0) {
        const int until = milliseconds_until(next_overview_, now);
        timeout = std::min(timeout, until);
      }
      for (const auto &slot : slots_) {
        if (slot && slot->session.stopping() && slot->session.active()) {
          int until = milliseconds_until(slot->session.shutdown_deadline(), now);
          timeout = timeout < 0 ? until : std::min(timeout, until);
        }
      }
      if (note_editor_ && note_editor_->session.stopping() && note_editor_->session.active())
        timeout = std::min(timeout, milliseconds_until(note_editor_->session.shutdown_deadline(), now));
      for (auto &tab : web_tabs_)
        if (tab && tab->browser && tab->browser->session.stopping() && tab->browser->session.active())
          timeout = std::min(timeout, milliseconds_until(tab->browser->session.shutdown_deadline(), now));
      if (escape_pending_) {
        int until = milliseconds_until(escape_deadline_, now);
        timeout = timeout < 0 ? until : std::min(timeout, until);
      }
      int result = ::poll(descriptors.data(), count, timeout);
      if (result < 0 && errno != EINTR) throw std::runtime_error("poll failed");
    }
  }

 private:
  enum class WebPromptKind { Wikipedia, Browse, DuckDuckGo, Google };

  const char *web_prompt_title() const {
    switch (web_prompt_kind_) {
      case WebPromptKind::Wikipedia: return "Wikipedia search";
      case WebPromptKind::Browse: return "Browse URL";
      case WebPromptKind::DuckDuckGo: return "DuckDuckGo search";
      case WebPromptKind::Google: return "Google search";
    }
    return "Web";
  }

  struct WebTab {
    std::unique_ptr<TerminalSlot> browser;
    bool browser_closing = false;
  };

  WebTab &web_tab() { return *web_tabs_[web_index_]; }
  const WebTab &web_tab() const { return *web_tabs_[web_index_]; }
  size_t web_position() const {
    return static_cast<size_t>(std::find(web_order_.begin(), web_order_.end(), web_index_) -
                               web_order_.begin());
  }
  bool any_web_browser() const {
    return std::any_of(web_tabs_.begin(), web_tabs_.end(), [](const auto &tab) {
      return tab && tab->browser != nullptr;
    });
  }

  void switch_web_tab(int direction) {
    const int next = static_cast<int>(web_position()) + direction;
    if (next >= 0 && next < static_cast<int>(web_order_.size())) {
      web_index_ = web_order_[static_cast<size_t>(next)];
      ui_dirty_ = true;
      return;
    }
    if (direction > 0) create_web_tab();
  }

  void create_web_tab() {
    for (size_t i = 0; i < web_tabs_.size(); ++i) {
      if (std::find(web_order_.begin(), web_order_.end(), i) != web_order_.end()) continue;
      if (!web_tabs_[i]) web_tabs_[i] = std::make_unique<WebTab>();
      web_order_.push_back(i);
      web_index_ = i;
      notice_.clear();
      ui_dirty_ = true;
      return;
    }
    notice_ = "all four Web tabs are in use";
    ui_dirty_ = true;
  }

  void close_web_tab() {
    if (web_order_.size() == 1) {
      notice_ = "Cannot close the last Web tab";
      ui_dirty_ = true;
      return;
    }
    auto &tab = web_tab();
    if (tab.browser) {
      tab.browser_closing = true;
      tab.browser->session.begin_shutdown();
    }
    const size_t position = web_position();
    web_order_.erase(web_order_.begin() + static_cast<std::ptrdiff_t>(position));
    web_index_ = web_order_[std::min(position, web_order_.size() - 1)];
    notice_ = "Web tab closed";
    ui_dirty_ = true;
  }

  void select_settings_section(size_t section) {
    if (bluetooth_page_ && section != 3 && bluetooth_view_.discovering)
      bluetooth_.set_discovering(false);
    settings_selection_ = section;
    wifi_page_ = section == 1;
    audio_page_ = section == 2;
    bluetooth_page_ = section == 3;
    if (wifi_page_) { wifi_view_ = wifi_.snapshot(); wifi_.scan(); }
    if (audio_page_) { audio_view_ = audio_.snapshot(); audio_.refresh(); }
    if (bluetooth_page_) { bluetooth_view_ = bluetooth_.snapshot(); bluetooth_.refresh(); }
    notice_.clear();
    ui_dirty_ = true;
  }

  static int milliseconds_until(Clock::time_point deadline, Clock::time_point now) {
    if (deadline <= now) return 0;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<int64_t>(duration + 1, std::numeric_limits<int>::max()));
  }

  void create_slot(size_t index) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int pane_rows = std::clamp(rows - layout_.reserved_rows(), 1, kMaxRows);
    int pane_cols = std::clamp(cols - 2 * layout_.inset, 1, kMaxCols);
    slots_[index] = std::make_unique<TerminalSlot>(shell_, pane_rows, pane_cols);
    active_index_ = index;
    ui_dirty_ = true;
  }

  TerminalSlot *active_slot() const { return slots_[active_index_].get(); }

  bool any_slots() const {
    return std::any_of(slots_.begin(), slots_.end(), [](const auto &slot) { return slot != nullptr; });
  }

  bool any_running() const {
    return std::any_of(slots_.begin(), slots_.end(), [](const auto &slot) {
      return slot && slot->session.active();
    });
  }

  void select_existing_slot() {
    for (size_t i = 0; i < kMaxSessions; ++i) {
      if (slots_[i]) {
        active_index_ = i;
        return;
      }
    }
    active_index_ = 0;
    active_tab_ = 1;
    menu_selection_ = 1;
    menu_focused_ = true;
  }

  void switch_to(size_t index) {
    if (!slots_[index]) {
      notice_ = "terminal " + std::to_string(index + 1) +
                " is empty; use Alt+l at the last tab";
    } else if (slots_[index]->closing) {
      notice_ = "terminal " + std::to_string(index + 1) + " is closing";
    } else {
      active_index_ = index;
      active_tab_ = 1;
      menu_selection_ = 1;
      menu_focused_ = false;
      notice_.clear();
    }
    ui_dirty_ = true;
  }

  void start_slot(size_t index) {
    try {
      create_slot(index);
      active_tab_ = 1;
      menu_selection_ = 1;
      menu_focused_ = false;
      notice_.clear();
    } catch (const std::exception &error) {
      notice_ = "cannot create terminal: " + std::string(error.what());
    }
    ui_dirty_ = true;
  }

  void create_new_slot() {
    size_t next = 0;
    size_t selected = active_index_;
    for (size_t i = 0; i < kMaxSessions; ++i) {
      if (!slots_[i]) continue;
      if (i == active_index_) selected = next;
      if (i != next) slots_[next] = std::move(slots_[i]);
      ++next;
    }
    active_index_ = selected;
    if (next < kMaxSessions) { start_slot(next); return; }
    notice_ = "all four terminal slots are in use";
    ui_dirty_ = true;
  }

  void refresh_notes() {
    const std::string selected = note_selection_ < notes_.size() ? notes_[note_selection_] : "";
    notes_ = list_notes(notes_directory());
    auto found = std::find(notes_.begin(), notes_.end(), selected);
    note_selection_ = found != notes_.end() ? static_cast<size_t>(found - notes_.begin()) :
        (notes_.empty() ? 0 : std::min(note_selection_, notes_.size() - 1));
  }

  void open_note(const std::string &name) {
    const auto directory = notes_directory();
    if (directory.empty()) { notice_ = "HOME or XDG_DATA_HOME is required"; return; }
    const std::string nano = find_nano();
    if (nano.empty()) { notice_ = "nano is not installed"; return; }
    try {
      int rows, cols;
      getmaxyx(stdscr, rows, cols);
      const int pane_rows = std::clamp(rows - layout_.reserved_rows(), 1, kMaxRows);
      const int pane_cols = std::clamp(cols - 2 * layout_.inset, 1, kMaxCols);
      note_editor_ = std::make_unique<TerminalSlot>(nano,
          std::vector<std::string>{(directory / name).string()}, pane_rows, pane_cols);
      notice_.clear();
    } catch (const std::exception &error) {
      notice_ = "Cannot open nano: " + std::string(error.what());
    }
    ui_dirty_ = true;
  }

  void create_note() {
    std::string name = note_name_;
    if (!valid_note_name(name)) { notice_ = "Enter a valid note name"; return; }
    if (name.size() < 4 || name.substr(name.size() - 4) != ".txt") name += ".txt";
    const auto directory = notes_directory();
    if (directory.empty()) { notice_ = "HOME or XDG_DATA_HOME is required"; return; }
    const std::string nano = find_nano();
    if (nano.empty()) { notice_ = "nano is not installed"; return; }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { notice_ = "Cannot create notes directory"; return; }
    const auto path = directory / name;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
      notice_ = errno == EEXIST ? "Note already exists" : "Cannot create note";
      return;
    }
    ::close(fd);
    note_name_prompt_ = false;
    note_name_.clear();
    refresh_notes();
    auto found = std::find(notes_.begin(), notes_.end(), name);
    if (found != notes_.end()) note_selection_ = static_cast<size_t>(found - notes_.begin());
    open_note(name);
  }

  void open_web_url(const std::string &url) {
    if ((url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) ||
        url.find_first_of(" \t\r\n") != std::string::npos) {
      notice_ = "Enter a valid HTTP or HTTPS URL";
      return;
    }
    if (web_tab().browser) {
      notice_ = web_tab().browser_closing ? "Previous page is closing" :
          "Close the current page with q first";
      return;
    }
    const std::string elinks = find_executable("elinks");
    if (elinks.empty()) { notice_ = "elinks is not installed"; return; }
    try {
      int rows, cols;
      getmaxyx(stdscr, rows, cols);
      const int pane_rows = std::clamp(rows - layout_.reserved_rows(), 1, kMaxRows);
      const int pane_cols = std::clamp(cols - 2 * layout_.inset, 1, kMaxCols);
      web_tab().browser = std::make_unique<TerminalSlot>(elinks,
          std::vector<std::string>{"-no-home", "-no-connect", "-eval",
                                   elinks_configuration(theme_, cookie_policy_, show_web_header_links_), url},
          pane_rows, pane_cols);
      notice_.clear();
    } catch (const std::exception &error) {
      notice_ = "Cannot open ELinks: " + std::string(error.what());
    }
    ui_dirty_ = true;
  }

  void resize() {
    bool changed = false;
    winsize size{};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col &&
        is_term_resized(size.ws_row, size.ws_col)) {
      (void)resizeterm(size.ws_row, size.ws_col);
      changed = true;
    }
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    const int pane_rows = rows - layout_.reserved_rows();
    const int pane_cols = cols - 2 * layout_.inset;
    if (pane_rows > 0 && pane_cols > 0 && pane_rows <= kMaxRows && pane_cols <= kMaxCols) {
      for (auto &slot : slots_) {
        if (!slot) continue;
        changed = changed || slot->model->rows() != pane_rows || slot->model->cols() != pane_cols;
        slot->model->resize(pane_rows, pane_cols);
      }
      if (note_editor_) {
        changed = changed || note_editor_->model->rows() != pane_rows ||
                  note_editor_->model->cols() != pane_cols;
        note_editor_->model->resize(pane_rows, pane_cols);
      }
      for (auto &tab : web_tabs_) {
        if (!tab || !tab->browser) continue;
        changed = changed || tab->browser->model->rows() != pane_rows ||
                  tab->browser->model->cols() != pane_cols;
        tab->browser->model->resize(pane_rows, pane_cols);
      }
    } else if (pane_rows > 0 && pane_cols > 0) {
      notice_ = "window exceeds 400 x 200 cell limit";
      changed = true;
    }
    if (changed) ui_dirty_ = true;
  }

  std::string status() const {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows <= layout_.reserved_rows() || cols <= 2 * layout_.inset) return "window too small";
    if (rows - layout_.reserved_rows() > kMaxRows || cols - 2 * layout_.inset > kMaxCols)
      return "window exceeds 400 x 200 cell limit";
    const TerminalSlot *selected = active_tab_ == 1 ? active_slot() :
                                   active_tab_ == 3 && notes_section_ == 0 ? note_editor_.get() :
                                   active_tab_ == 4 && !web_tab().browser_closing ?
                                       web_tab().browser.get() : nullptr;
    if (debug_input_)
      return "INPUT " + std::to_string(input_events_) + " / PTY " +
             std::to_string(forwarded_events_);
    if (!notice_.empty()) return notice_;
    if (selected && selected->session.free_queue() == 0) return "input queue full";
    return {};
  }

  ModalView modal() const {
    if (help_)
      return {ModalView::Kind::Info, {}, {}, {}, false};
    if (confirm_ == Confirm::Quit)
      return {ModalView::Kind::Confirm, "Exit Cyber Deck and close all terminals?", {},
              "Yes", modal_accept_selected_};
    if (confirm_ == Confirm::CloseSlot)
      return {ModalView::Kind::Confirm,
              "Close terminal " + std::to_string(pending_close_ + 1) + "?", {},
              "Yes", modal_accept_selected_};
    if (wifi_forget_prompt_)
      return {ModalView::Kind::Confirm,
              "Forget all profiles for " + wifi_forget_name_ + "?",
              "Disconnects this network if active.", "Yes", modal_accept_selected_};
    if (bluetooth_forget_prompt_)
      return {ModalView::Kind::Confirm,
              "Forget " + bluetooth_forget_name_ + "?", "Removes its Bluetooth pairing.",
              "Yes", modal_accept_selected_};
    if (wifi_password_prompt_)
      return {ModalView::Kind::Password,
              (wifi_pending_.saved_uuid.empty() ? "Enter " : "Replace ") +
                  std::string(wifi_pending_.uses_sae ? "SAE" : "PSK") +
                  " password for " + wifi_pending_.ssid,
              notice_.empty() ? "Password: " + std::string(std::count_if(
                  wifi_password_.begin(), wifi_password_.end(), [](unsigned char ch) {
                    return (ch & 0xC0) != 0x80;
                  }), '*') : notice_,
              wifi_pending_.saved_uuid.empty() ? "Connect" : "Save", modal_accept_selected_};
    if (bluetooth_view_.prompt != BluetoothPromptKind::None &&
        bluetooth_answered_epoch_ != bluetooth_view_.prompt_epoch) {
      if (bluetooth_view_.prompt == BluetoothPromptKind::Confirm) {
        std::string code;
        if (bluetooth_view_.prompt_has_passkey) {
          char digits[7];
          std::snprintf(digits, sizeof(digits), "%06u", bluetooth_view_.prompt_passkey);
          code = std::string("Confirm code ") + digits + " on both devices";
        } else code = "Allow this Bluetooth pairing?";
        return {ModalView::Kind::Confirm,
                "Pair with " + bluetooth_view_.prompt_device + "?", code,
                "Yes", modal_accept_selected_};
      }
      const bool passkey = bluetooth_view_.prompt == BluetoothPromptKind::Passkey;
      return {ModalView::Kind::Password,
              std::string(passkey ? "Enter passkey for " : "Enter PIN for ") +
                  bluetooth_view_.prompt_device,
              notice_.empty() ? std::string(passkey ? "Passkey: " : "PIN: ") +
                  std::string(bluetooth_code_.size(), '*') : notice_,
              "Pair", modal_accept_selected_};
    }
    if (wave_add_stage_ != WaveAddStage::None) {
      const bool name_stage = wave_add_stage_ == WaveAddStage::Name;
      const std::string &entry = name_stage ? wave_name_ : wave_url_;
      const std::string visible = entry.size() > 43 ? "..." + entry.substr(entry.size() - 40) : entry;
      return {ModalView::Kind::TextEntry,
              notice_.empty() ? (name_stage ? "Add station: name" : "Add station: stream URL") : notice_,
              std::string(name_stage ? "Name: " : "URL: ") + visible + "_",
              name_stage ? "Enter next   Alt+Esc cancel" : "Enter save   Alt+Esc cancel", false};
    }
    if (note_name_prompt_)
      return {ModalView::Kind::TextEntry,
              notice_.empty() ? "New note" : notice_,
              "Name: " + note_name_ + "_", "Enter create   Alt+Esc cancel", false};
    if (web_query_prompt_)
      return {ModalView::Kind::TextEntry,
              notice_.empty() ? web_prompt_title() : notice_,
              std::string(web_prompt_kind_ == WebPromptKind::Browse ? "URL: " : "Query: ") +
                  (web_query_.size() > 42 ? "..." + web_query_.substr(web_query_.size() - 39) : web_query_) + "_",
              "Enter open   Alt+Esc cancel", false};
    if (audio_picker_)
      return {ModalView::Kind::AudioPicker, {}, {}, {}, false};
    return {};
  }

  void request_quit(bool from_signal = false) {
    help_ = false;
    wave_add_stage_ = WaveAddStage::None;
    note_name_prompt_ = false;
    web_query_prompt_ = false;
    if (wifi_password_prompt_) clear_wifi_password();
    wifi_forget_prompt_ = false;
    bluetooth_forget_prompt_ = false;
    if (bluetooth_view_.prompt != BluetoothPromptKind::None &&
        bluetooth_answered_epoch_ != bluetooth_view_.prompt_epoch) {
      bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, false);
      bluetooth_answered_epoch_ = bluetooth_view_.prompt_epoch;
    }
    if (!from_signal && (any_running() || (note_editor_ && note_editor_->session.active()) ||
                         any_web_browser())) {
      confirm_ = Confirm::Quit;
      modal_accept_selected_ = false;
      ui_dirty_ = true;
      return;
    }
    start_quit();
  }

  void start_quit() {
    quitting_ = true;
    confirm_ = Confirm::None;
    for (auto &slot : slots_) {
      if (!slot) continue;
      slot->closing = true;
      slot->session.begin_shutdown();
    }
    if (note_editor_) note_editor_->session.begin_shutdown();
    for (auto &tab : web_tabs_)
      if (tab && tab->browser) tab->browser->session.begin_shutdown();
    ui_dirty_ = true;
  }

  void request_close_slot() {
    if (active_tab_ == 4 && !menu_focused_) {
      close_web_tab();
      return;
    }
    if (active_tab_ != 1 || menu_focused_) {
      notice_ = "open Terminal or Web to close a tab"; ui_dirty_ = true; return;
    }
    size_t index = active_index_;
    TerminalSlot *slot = slots_[index].get();
    if (!slot || slot->closing) return;
    if (std::count_if(slots_.begin(), slots_.end(), [](const auto &entry) {
          return entry != nullptr;
        }) == 1) {
      notice_ = "Cannot close the last Terminal tab";
      ui_dirty_ = true;
      return;
    }
    if (slot->session.active()) {
      pending_close_ = index;
      confirm_ = Confirm::CloseSlot;
      modal_accept_selected_ = false;
    } else {
      slot->closing = true;
    }
    ui_dirty_ = true;
  }

  void accept_confirmation() {
    if (confirm_ == Confirm::Quit) start_quit();
    else if (confirm_ == Confirm::CloseSlot) {
      if (slots_[pending_close_]) {
        slots_[pending_close_]->closing = true;
        slots_[pending_close_]->session.begin_shutdown();
      }
      confirm_ = Confirm::None;
      ui_dirty_ = true;
    } else if (wifi_forget_prompt_) {
      wifi_forget_start_epoch_ = wifi_view_.forget_epoch;
      wifi_forgetting_request_ = true;
      wifi_.forget(wifi_forget_uuid_);
      wifi_forget_prompt_ = false;
      wifi_forget_uuid_.clear();
      wifi_forget_name_.clear();
      notice_.clear();
      ui_dirty_ = true;
    } else if (bluetooth_forget_prompt_) {
      bluetooth_.forget(bluetooth_forget_path_);
      bluetooth_forget_prompt_ = false;
      bluetooth_forget_path_.clear();
      bluetooth_forget_name_.clear();
      notice_.clear();
      ui_dirty_ = true;
    }
  }

  void cancel_confirmation() {
    confirm_ = Confirm::None;
    wifi_forget_prompt_ = false;
    wifi_forget_uuid_.clear();
    wifi_forget_name_.clear();
    bluetooth_forget_prompt_ = false;
    bluetooth_forget_path_.clear();
    bluetooth_forget_name_.clear();
    ui_dirty_ = true;
  }

  void cyberdeck_back() {
    const bool bluetooth_prompt_active = bluetooth_view_.prompt != BluetoothPromptKind::None &&
        bluetooth_answered_epoch_ != bluetooth_view_.prompt_epoch;
    if (help_ || confirm_ != Confirm::None || wifi_forget_prompt_ ||
        wifi_password_prompt_ || bluetooth_forget_prompt_ || bluetooth_prompt_active ||
        audio_picker_ || wave_add_stage_ != WaveAddStage::None ||
        note_name_prompt_ || web_query_prompt_) {
      handle_plain({27, false});
      return;
    }
    if (!menu_focused_) {
      menu_focused_ = true;
      notice_.clear();
      ui_dirty_ = true;
    }
  }

  bool handle_alt(wint_t value) {
    const bool bluetooth_prompt_active = bluetooth_view_.prompt != BluetoothPromptKind::None &&
        bluetooth_answered_epoch_ != bluetooth_view_.prompt_epoch;
    const bool overlay_open = confirm_ != Confirm::None || wifi_forget_prompt_ ||
                              wifi_password_prompt_ || bluetooth_forget_prompt_ ||
                              bluetooth_prompt_active || help_ || audio_picker_ ||
                              wave_add_stage_ != WaveAddStage::None || note_name_prompt_ ||
                              web_query_prompt_;
    if (value >= 'A' && value <= 'Z') value = value - 'A' + 'a';
    if (value == 27) { cyberdeck_back(); return true; }
    if (value == '?') {
      help_ = !help_;
      ui_dirty_ = true;
      return true;
    }
    if (help_) {
      const bool module_shortcut = value == 'd' || value == 't' || value == 'm' ||
                                   value == 'n' || value == 'w' || value == 's';
      const bool confirmation = confirm_ != Confirm::None || wifi_forget_prompt_ ||
                                bluetooth_forget_prompt_ ||
                                (bluetooth_prompt_active &&
                                 bluetooth_view_.prompt == BluetoothPromptKind::Confirm);
      if (!module_shortcut && value != 'q' &&
          !(confirmation && (value == 'y' || value == 'n'))) return true;
      help_ = false;
      ui_dirty_ = true;
    }
    if (value == 'q') { request_quit(); return true; }
    if (bluetooth_prompt_active) {
      if (value == 'n') {
        bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, false);
        bluetooth_answered_epoch_ = bluetooth_view_.prompt_epoch;
        bluetooth_code_.clear();
        ui_dirty_ = true;
      } else if (value == 'y' && bluetooth_view_.prompt == BluetoothPromptKind::Confirm) {
        bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, true);
        bluetooth_answered_epoch_ = bluetooth_view_.prompt_epoch;
        ui_dirty_ = true;
      }
      if (value != 'd' && value != 't' && value != 'm' &&
          value != 'w' && value != 's') return true;
    }
    if (confirm_ != Confirm::None || wifi_forget_prompt_ || bluetooth_forget_prompt_) {
      if (value == 'y') { accept_confirmation(); return true; }
      if (value == 'n') { cancel_confirmation(); return true; }
    }
    if (menu_focused_ && !overlay_open && (value == 'h' || value == 'l')) {
      if (value == 'h' && menu_selection_ > 0) --menu_selection_;
      if (value == 'l' && menu_selection_ + 1 < kTabs.size()) ++menu_selection_;
      active_tab_ = menu_selection_;
      notice_.clear();
      ui_dirty_ = true;
      return true;
    }
    if (!menu_focused_ && !overlay_open && (value == 'h' || value == 'l')) {
      const int direction = value == 'h' ? -1 : 1;
      if (active_tab_ == 1) {
        bool moved = false;
        for (int index = static_cast<int>(active_index_) + direction;
             index >= 0 && index < static_cast<int>(kMaxSessions); index += direction) {
          if (slots_[index]) {
            active_index_ = static_cast<size_t>(index);
            ui_dirty_ = true;
            moved = true;
            break;
          }
        }
        if (!moved && direction > 0) create_new_slot();
      } else if (active_tab_ == 3) {
        const int next = static_cast<int>(notes_section_) + direction;
        if (next >= 0 && next < 2) { notes_section_ = static_cast<size_t>(next); ui_dirty_ = true; }
      } else if (active_tab_ == 4) {
        switch_web_tab(direction);
      } else if (active_tab_ == 5) {
        const int next = static_cast<int>(settings_selection_) + direction;
        if (next >= 0 && next < static_cast<int>(kSettingsSections.size()))
          select_settings_section(static_cast<size_t>(next));
      }
      return true;
    }
    auto open_tab = [this](size_t tab) {
      wave_add_stage_ = WaveAddStage::None;
      wave_name_.clear();
      wave_url_.clear();
      note_name_prompt_ = false;
      note_name_.clear();
      web_query_prompt_ = false;
      web_query_.clear();
      if (bluetooth_page_ && tab != 5 && bluetooth_view_.discovering)
        bluetooth_.set_discovering(false);
      if (bluetooth_view_.prompt != BluetoothPromptKind::None &&
          bluetooth_answered_epoch_ != bluetooth_view_.prompt_epoch) {
        bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, false);
        bluetooth_answered_epoch_ = bluetooth_view_.prompt_epoch;
      }
      if (wifi_password_prompt_) clear_wifi_password();
      wifi_forget_prompt_ = false;
      wifi_forget_uuid_.clear();
      wifi_forget_name_.clear();
      bluetooth_forget_prompt_ = false;
      bluetooth_forget_path_.clear();
      bluetooth_forget_name_.clear();
      confirm_ = Confirm::None;
      help_ = false;
      audio_picker_ = false;
      active_tab_ = tab;
      menu_selection_ = tab;
      menu_focused_ = false;
      if (tab == 5) {
        wifi_page_ = audio_page_ = bluetooth_page_ = false;
        select_settings_section(settings_selection_);
      }
      if (tab == 3) refresh_notes();
      notice_.clear();
      ui_dirty_ = true;
    };
    if (value == 'd') { open_tab(0); return true; }
    if (value == 't') { open_tab(1); return true; }
    if (value == 'm') { open_tab(2); return true; }
    if (value == 'n') { open_tab(3); return true; }
    if (value == 'w') { open_tab(4); return true; }
    if (value == 's') { open_tab(5); return true; }
    if (overlay_open) return false;
    if (value >= '1' && value <= '4') {
      help_ = false;
      switch_to(value - '1');
    } else if (value == 'x') {
      help_ = false;
      request_close_slot();
    } else {
      if (!menu_focused_ && (active_tab_ == 1 ||
                             (active_tab_ == 3 && notes_section_ == 0 && note_editor_) ||
                             (active_tab_ == 4 && web_tab().browser))) {
        forward_to_selected({27, false});
        forward_to_selected({value, false});
        return true;
      }
      return true;
    }
    return true;
  }

  void forward_to_selected(Key key) {
    if (active_tab_ == 4 && web_tab().browser_closing) return;
    TerminalSlot *slot = active_tab_ == 3 ? note_editor_.get() :
                         active_tab_ == 4 ? web_tab().browser.get() : active_slot();
    if (!slot || !slot->session.active() || slot->session.stopping()) return;
    // Leave room for terminal query replies. A held paste cannot grow memory.
    if (slot->session.free_queue() < 1024) {
      notice_ = "input backpressure";
      ui_dirty_ = true;
      return;
    }
    size_t free_before = slot->session.free_queue();
    forward_key(*slot->model, key);
    if (slot->session.free_queue() < free_before) ++forwarded_events_;
  }

  void clear_wifi_password() {
    std::fill(wifi_password_.begin(), wifi_password_.end(), '\0');
    wifi_password_.clear();
    wifi_password_prompt_ = false;
    wifi_pending_ = {};
    ui_dirty_ = true;
  }

  bool move_modal_choice(Key key, bool letter_keys) {
    if ((key.special && key.value == KEY_LEFT) ||
        (letter_keys && !key.special && key.value == 'h')) {
      modal_accept_selected_ = true;
    } else if ((key.special && key.value == KEY_RIGHT) ||
               (letter_keys && !key.special && key.value == 'l')) {
      modal_accept_selected_ = false;
    } else if (!key.special && key.value == 9) {
      modal_accept_selected_ = !modal_accept_selected_;
    } else return false;
    ui_dirty_ = true;
    return true;
  }

  void handle_audio_key(Key key) {
    const bool enter = (key.special && key.value == KEY_ENTER) ||
        (!key.special && (key.value == 10 || key.value == 13));
    const bool up = (key.special && key.value == KEY_UP) ||
        (!key.special && key.value == 'k');
    const bool down = (key.special && key.value == KEY_DOWN) ||
        (!key.special && key.value == 'j');
    const bool left = key.special && key.value == KEY_LEFT;
    const bool right = key.special && key.value == KEY_RIGHT;
    if (audio_view_.volume_only) {
      if (!key.special && key.value == 'r') audio_.refresh();
      else if ((left || right) && audio_view_.available && !audio_view_.outputs.empty()) {
        auto &music = audio_view_.outputs.front();
        const unsigned volume = std::min(music.volume, 100u);
        music.volume = right ? std::min(volume + 5, 100u) :
                               (volume >= 5 ? volume - 5 : 0);
        audio_.set_volume(true, music.name, music.volume);
      }
      ui_dirty_ = true;
      return;
    }
    const bool output = audio_selection_ == 0;
    auto &devices = output ? audio_view_.outputs : audio_view_.inputs;
    if (!key.special && key.value == 27) {
      if (audio_picker_) audio_picker_ = false;
      else menu_focused_ = true;
      notice_.clear();
    } else if (!key.special && key.value == 'r') {
      audio_.refresh();
    } else if (audio_picker_) {
      if (up && audio_picker_selection_ > 0) --audio_picker_selection_;
      else if (down && audio_picker_selection_ + 1 < devices.size()) ++audio_picker_selection_;
      else if (enter && audio_view_.available && audio_picker_selection_ < devices.size()) {
        const std::string name = devices[audio_picker_selection_].name;
        audio_.set_default(output, name);
        for (auto &candidate : devices) candidate.is_default = candidate.name == name;
        audio_picker_ = false;
        notice_.clear();
      }
    } else if (up) {
      audio_selection_ = 0;
    } else if (down) {
      audio_selection_ = 1;
    } else if (enter && audio_view_.available) {
      audio_picker_ = true;
      auto current = std::find_if(devices.begin(), devices.end(),
          [](const AudioDevice &device) { return device.is_default; });
      audio_picker_selection_ = current == devices.end() ? 0 :
          static_cast<size_t>(current - devices.begin());
    } else if ((left || right) && audio_view_.available) {
      auto current = std::find_if(devices.begin(), devices.end(),
          [](const AudioDevice &device) { return device.is_default; });
      if (current != devices.end()) {
        const unsigned volume = std::min(current->volume, 100u);
        current->volume = right ? std::min(volume + 5, 100u) :
                                  (volume >= 5 ? volume - 5 : 0);
        audio_.set_volume(output, current->name, current->volume);
        notice_.clear();
      }
    }
    ui_dirty_ = true;
  }

  void handle_bluetooth_prompt_key(Key key) {
    const bool enter = (key.special && key.value == KEY_ENTER) ||
        (!key.special && (key.value == 10 || key.value == 13));
    const bool confirm = bluetooth_view_.prompt == BluetoothPromptKind::Confirm;
    if (move_modal_choice(key, confirm)) return;
    if (!key.special && key.value == 27) {
      bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, false);
    } else if (enter) {
      if (!modal_accept_selected_) {
        bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, false);
      } else if (confirm) {
        bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, true);
      } else {
        const bool passkey = bluetooth_view_.prompt == BluetoothPromptKind::Passkey;
        const bool valid = !bluetooth_code_.empty() &&
            bluetooth_code_.size() <= (passkey ? 6u : 16u) &&
            (!passkey || std::all_of(bluetooth_code_.begin(), bluetooth_code_.end(),
                [](unsigned char ch) { return ch >= '0' && ch <= '9'; }));
        if (!valid) {
          notice_ = passkey ? "Passkey needs 1-6 digits" : "PIN needs 1-16 characters";
          ui_dirty_ = true;
          return;
        }
        bluetooth_.answer_prompt(bluetooth_view_.prompt_epoch, true, bluetooth_code_);
      }
    } else if (!confirm && ((key.special && key.value == KEY_BACKSPACE) ||
                            (!key.special && (key.value == 8 || key.value == 127)))) {
      if (!bluetooth_code_.empty()) bluetooth_code_.pop_back();
      notice_.clear();
      ui_dirty_ = true;
      return;
    } else if (!confirm && !key.special && key.value >= 32 && key.value <= 126) {
      const bool passkey = bluetooth_view_.prompt == BluetoothPromptKind::Passkey;
      if (bluetooth_code_.size() < (passkey ? 6u : 16u) &&
          (!passkey || (key.value >= '0' && key.value <= '9')))
        bluetooth_code_.push_back(static_cast<char>(key.value));
      notice_.clear();
      ui_dirty_ = true;
      return;
    } else return;
    bluetooth_answered_epoch_ = bluetooth_view_.prompt_epoch;
    bluetooth_code_.clear();
    notice_.clear();
    ui_dirty_ = true;
  }

  void handle_bluetooth_key(Key key) {
    const bool enter = (key.special && key.value == KEY_ENTER) ||
        (!key.special && (key.value == 10 || key.value == 13));
    const bool up = (key.special && key.value == KEY_UP) ||
        (!key.special && key.value == 'k');
    const bool down = (key.special && key.value == KEY_DOWN) ||
        (!key.special && key.value == 'j');
    if (bluetooth_view_.info_only) {
      if (!key.special && key.value == 'r') bluetooth_.refresh();
      ui_dirty_ = true;
      return;
    }
    if (!key.special && key.value == 27) {
      if (bluetooth_view_.discovering) bluetooth_.set_discovering(false);
      menu_focused_ = true;
      notice_.clear();
    } else if (up && bluetooth_selection_ > 0) --bluetooth_selection_;
    else if (down && bluetooth_selection_ + 1 < bluetooth_view_.devices.size())
      ++bluetooth_selection_;
    else if (bluetooth_view_.busy && (enter || (!key.special &&
             (key.value == 'r' || key.value == 'd' || key.value == 'f' ||
              key.value == 'p' || key.value == 't' || key.value == 'w'))))
      notice_ = "Wait for Bluetooth operation to finish";
    else if (!key.special && key.value == 'w' && bluetooth_view_.available) {
      bluetooth_.set_powered(!bluetooth_view_.powered);
      notice_.clear();
    } else if (!key.special && key.value == 'r') {
      if (!bluetooth_view_.available) bluetooth_.refresh();
      else if (!bluetooth_view_.powered) notice_ = "Power on Bluetooth first";
      else { bluetooth_.set_discovering(!bluetooth_view_.discovering); notice_.clear(); }
    } else if (bluetooth_selection_ < bluetooth_view_.devices.size()) {
      const auto &device = bluetooth_view_.devices[bluetooth_selection_];
      const bool action = enter || (!key.special &&
          (key.value == 'p' || key.value == 'd' || key.value == 't' || key.value == 'f'));
      if (action && !bluetooth_view_.powered) {
        notice_ = "Power on Bluetooth first";
      } else if (enter) {
        if (device.connected) notice_ = "Already connected";
        else if (device.paired) { bluetooth_.connect(device.path); notice_.clear(); }
        else { bluetooth_.pair(device.path, true); notice_.clear(); }
      } else if (!key.special && key.value == 'p') {
        if (device.paired) notice_ = "Already paired";
        else { bluetooth_.pair(device.path); notice_.clear(); }
      } else if (!key.special && key.value == 'd') {
        if (!device.connected) notice_ = "Device is not connected";
        else { bluetooth_.disconnect(device.path); notice_.clear(); }
      } else if (!key.special && key.value == 't') {
        bluetooth_.set_trusted(device.path, !device.trusted);
        notice_.clear();
      } else if (!key.special && key.value == 'f') {
        if (!device.paired) notice_ = "This device is not paired";
        else {
          bluetooth_forget_path_ = device.path;
          bluetooth_forget_name_ = device.name;
          bluetooth_forget_prompt_ = true;
          modal_accept_selected_ = false;
          notice_.clear();
        }
      }
    }
    ui_dirty_ = true;
  }

  void handle_wifi_key(Key key) {
    const bool enter = (key.special && key.value == KEY_ENTER) ||
        (!key.special && (key.value == 10 || key.value == 13));
    if (wifi_view_.read_only) {
      if ((key.special && key.value == KEY_UP) || (!key.special && key.value == 'k')) {
        if (wifi_selection_ > 0) --wifi_selection_;
      } else if ((key.special && key.value == KEY_DOWN) || (!key.special && key.value == 'j')) {
        if (wifi_selection_ + 1 < wifi_view_.networks.size()) ++wifi_selection_;
      } else if (!key.special && key.value == 'r') wifi_.scan();
      else if (enter || (!key.special &&
               (key.value == 'd' || key.value == 'w' || key.value == 'p' || key.value == 'f')))
        notice_ = "WiFi management is unavailable through Termux:API";
      ui_dirty_ = true;
      return;
    }
    if (wifi_forget_prompt_) {
      if (move_modal_choice(key, true)) return;
      if (enter && modal_accept_selected_) accept_confirmation();
      else if (enter || (!key.special && key.value == 27)) cancel_confirmation();
      return;
    }
    if (wifi_password_prompt_) {
      if (move_modal_choice(key, false)) return;
      if (enter) {
        if (!modal_accept_selected_) {
          clear_wifi_password();
          notice_.clear();
          return;
        }
        const size_t length = wifi_password_.size();
        const bool printable_ascii = std::all_of(wifi_password_.begin(), wifi_password_.end(),
            [](unsigned char ch) { return ch >= 32 && ch <= 126; });
        const bool hex_key = length == 64 &&
            std::all_of(wifi_password_.begin(), wifi_password_.end(), [](unsigned char ch) {
              return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                     (ch >= 'A' && ch <= 'F');
            });
        const bool valid_psk = (length >= 8 && length <= 63 && printable_ascii) || hex_key;
        if (wifi_pending_.uses_sae ? length == 0 : !valid_psk) {
          notice_ = wifi_pending_.uses_sae ? "SAE password cannot be empty" :
              "PSK needs 8-63 ASCII characters or 64 hex digits";
          ui_dirty_ = true;
        } else {
          wifi_.connect(wifi_pending_, wifi_password_);
          notice_.clear();
          clear_wifi_password();
        }
      } else if (!key.special && key.value == 27) {
        clear_wifi_password();
        notice_.clear();
      } else if ((key.special && key.value == KEY_BACKSPACE) ||
                 (!key.special && (key.value == 8 || key.value == 127))) {
        if (!wifi_password_.empty()) {
          do { wifi_password_.pop_back(); }
          while (!wifi_password_.empty() &&
                 (static_cast<unsigned char>(wifi_password_.back()) & 0xC0) == 0x80);
          notice_.clear();
          ui_dirty_ = true;
        }
      } else if (!key.special && key.value >= 32 && key.value <= WCHAR_MAX) {
        std::mbstate_t state{};
        char encoded[MB_LEN_MAX]{};
        const size_t length = std::wcrtomb(encoded, static_cast<wchar_t>(key.value), &state);
        if (length != static_cast<size_t>(-1)) {
          const size_t limit = wifi_pending_.uses_sae ? 1024 : 64;
          if (wifi_password_.size() + length <= limit) {
            wifi_password_.append(encoded, length);
            notice_.clear();
          } else {
            notice_ = wifi_pending_.uses_sae ? "SAE password limit: 1024 bytes" :
                "PSK password limit: 64 bytes";
          }
          ui_dirty_ = true;
        }
      }
      return;
    }
    if ((wifi_forgetting_request_ || wifi_view_.forgetting) &&
        (enter || (!key.special && key.value == 'f'))) {
      notice_ = "Wait for Forget to finish";
      ui_dirty_ = true;
      return;
    }
    if (enter && wifi_selection_ < wifi_view_.networks.size()) {
      const auto &network = wifi_view_.networks[wifi_selection_];
      if (network.connected) notice_ = "Already connected";
      else if (!network.visible) notice_ = "Network out of range; press r to scan";
      else if (network.security == WifiSecurity::Enterprise ||
               network.security == WifiSecurity::Unsupported) notice_ = "This network needs advanced setup";
      else if (network.security == WifiSecurity::Personal && network.saved_uuid.empty()) {
        wifi_pending_ = network;
        wifi_password_prompt_ = true;
        modal_accept_selected_ = true;
        notice_.clear();
      } else {
        wifi_.connect(network);
        notice_.clear();
      }
      ui_dirty_ = true;
    } else if ((key.special && key.value == KEY_UP) || (!key.special && key.value == 'k')) {
      if (wifi_selection_ > 0) --wifi_selection_;
      ui_dirty_ = true;
    } else if ((key.special && key.value == KEY_DOWN) || (!key.special && key.value == 'j')) {
      if (wifi_selection_ + 1 < wifi_view_.networks.size()) ++wifi_selection_;
      ui_dirty_ = true;
    } else if (!key.special && key.value == 'r') {
      wifi_.scan();
      notice_.clear();
      ui_dirty_ = true;
    } else if (!key.special && key.value == 'd') {
      wifi_.disconnect();
      notice_.clear();
      ui_dirty_ = true;
    } else if (!key.special && key.value == 'w') {
      wifi_.set_enabled(!wifi_view_.enabled);
      notice_.clear();
      ui_dirty_ = true;
    } else if (!key.special && key.value == 'p' && wifi_selection_ < wifi_view_.networks.size()) {
      const auto &network = wifi_view_.networks[wifi_selection_];
      if (!network.visible) notice_ = "Network out of range; press r to scan";
      else if (network.security != WifiSecurity::Personal || network.saved_uuid.empty())
        notice_ = "Select a saved password-protected network";
      else {
        wifi_pending_ = network;
        wifi_password_prompt_ = true;
        modal_accept_selected_ = true;
        notice_.clear();
      }
      ui_dirty_ = true;
    } else if (!key.special && key.value == 'f' && wifi_selection_ < wifi_view_.networks.size()) {
      const auto &network = wifi_view_.networks[wifi_selection_];
      if (network.saved_uuid.empty()) notice_ = "This network has no saved profile";
      else {
        wifi_forget_uuid_ = network.saved_uuid;
        wifi_forget_name_ = network.ssid;
        wifi_forget_prompt_ = true;
        modal_accept_selected_ = false;
        notice_.clear();
      }
      ui_dirty_ = true;
    } else if (!key.special && key.value == 27) {
      menu_focused_ = true;
      notice_.clear();
      ui_dirty_ = true;
    }
  }

  void handle_wave_add_key(Key key) {
    const bool enter = (key.special && key.value == KEY_ENTER) ||
        (!key.special && (key.value == 10 || key.value == 13));
    if (!key.special && key.value == 27) {
      wave_add_stage_ = WaveAddStage::None;
      wave_name_.clear();
      wave_url_.clear();
      notice_.clear();
    } else if (enter) {
      if (wave_add_stage_ == WaveAddStage::Name) {
        if (wave_name_.empty()) notice_ = "Enter a station name";
        else { wave_add_stage_ = WaveAddStage::Url; notice_.clear(); }
      } else {
        std::string error;
        if (wave_.add_station(wave_name_, wave_url_, error)) {
          wave_selection_ = wave_.stations().size() - 1;
          wave_add_stage_ = WaveAddStage::None;
          notice_ = "Added " + wave_name_;
          wave_name_.clear();
          wave_url_.clear();
        } else notice_ = std::move(error);
      }
    } else if ((key.special && key.value == KEY_BACKSPACE) ||
               (!key.special && (key.value == 8 || key.value == 127))) {
      std::string &entry = wave_add_stage_ == WaveAddStage::Name ? wave_name_ : wave_url_;
      if (!entry.empty()) {
        do { entry.pop_back(); }
        while (!entry.empty() && (static_cast<unsigned char>(entry.back()) & 0xC0) == 0x80);
      }
      notice_.clear();
    } else if (!key.special && key.value == 21) {
      (wave_add_stage_ == WaveAddStage::Name ? wave_name_ : wave_url_).clear();
      notice_.clear();
    } else if (!key.special && key.value >= 32 && key.value <= WCHAR_MAX) {
      std::mbstate_t state{};
      char encoded[MB_LEN_MAX]{};
      const size_t length = std::wcrtomb(encoded, static_cast<wchar_t>(key.value), &state);
      std::string &entry = wave_add_stage_ == WaveAddStage::Name ? wave_name_ : wave_url_;
      const size_t limit = wave_add_stage_ == WaveAddStage::Name ? 120 : 2048;
      if (length != static_cast<size_t>(-1) && entry.size() + length <= limit) {
        entry.append(encoded, length);
        notice_.clear();
      }
    }
    ui_dirty_ = true;
  }

  void handle_note_name_key(Key key) {
    const bool enter = (key.special && key.value == KEY_ENTER) ||
        (!key.special && (key.value == 10 || key.value == 13));
    if (!key.special && key.value == 27) {
      note_name_prompt_ = false;
      note_name_.clear();
      notice_.clear();
    } else if (enter) {
      create_note();
    } else if ((key.special && key.value == KEY_BACKSPACE) ||
               (!key.special && (key.value == 8 || key.value == 127))) {
      if (!note_name_.empty()) {
        do { note_name_.pop_back(); }
        while (!note_name_.empty() && (static_cast<unsigned char>(note_name_.back()) & 0xC0) == 0x80);
      }
      notice_.clear();
    } else if (!key.special && key.value == 21) {
      note_name_.clear();
      notice_.clear();
    } else if (!key.special && key.value >= 32 && key.value <= WCHAR_MAX) {
      std::mbstate_t state{};
      char encoded[MB_LEN_MAX]{};
      const size_t length = std::wcrtomb(encoded, static_cast<wchar_t>(key.value), &state);
      if (length != static_cast<size_t>(-1) && note_name_.size() + length <= 100) {
        note_name_.append(encoded, length);
        notice_.clear();
      }
    }
    ui_dirty_ = true;
  }

  void handle_web_query_key(Key key) {
    const bool enter = (key.special && key.value == KEY_ENTER) ||
        (!key.special && (key.value == 10 || key.value == 13));
    if (!key.special && key.value == 27) {
      web_query_prompt_ = false;
      web_query_.clear();
      notice_.clear();
    } else if (enter) {
      if (web_query_.empty()) notice_ = web_prompt_kind_ == WebPromptKind::Browse ?
          "Enter a URL" : "Enter a search query";
      else {
        std::string url;
        if (web_prompt_kind_ == WebPromptKind::Browse) {
          url = web_query_;
          if (url.find("://") == std::string::npos) url = "https://" + url;
        } else {
          const std::string query = encode_web_query(web_query_);
          if (web_prompt_kind_ == WebPromptKind::Wikipedia)
            url = "https://en.wikipedia.org/w/index.php?search=" + query;
          else if (web_prompt_kind_ == WebPromptKind::DuckDuckGo)
            url = "https://lite.duckduckgo.com/lite/?q=" + query;
          else
            url = "https://www.google.com/search?q=" + query;
        }
        open_web_url(url);
        if (web_tab().browser) {
          web_query_prompt_ = false;
          web_query_.clear();
        }
      }
    } else if ((key.special && key.value == KEY_BACKSPACE) ||
               (!key.special && (key.value == 8 || key.value == 127))) {
      if (!web_query_.empty()) {
        do { web_query_.pop_back(); }
        while (!web_query_.empty() && (static_cast<unsigned char>(web_query_.back()) & 0xC0) == 0x80);
      }
      notice_.clear();
    } else if (!key.special && key.value == 21) {
      web_query_.clear();
      notice_.clear();
    } else if (!key.special && key.value >= 32 && key.value <= WCHAR_MAX) {
      std::mbstate_t state{};
      char encoded[MB_LEN_MAX]{};
      const size_t length = std::wcrtomb(encoded, static_cast<wchar_t>(key.value), &state);
      if (length != static_cast<size_t>(-1) && web_query_.size() + length <= 200) {
        web_query_.append(encoded, length);
        notice_.clear();
      }
    }
    ui_dirty_ = true;
  }

  void handle_plain(Key key) {
    if (help_) {
      if (!key.special && key.value == 27) {
        help_ = false;
        ui_dirty_ = true;
      }
      return;
    }
    if (bluetooth_forget_prompt_) {
      if (move_modal_choice(key, true)) return;
      const bool enter = (key.special && key.value == KEY_ENTER) ||
          (!key.special && (key.value == 10 || key.value == 13));
      if (enter && modal_accept_selected_) accept_confirmation();
      else if (enter || (!key.special && key.value == 27)) cancel_confirmation();
      return;
    }
    if (bluetooth_view_.prompt != BluetoothPromptKind::None &&
        bluetooth_answered_epoch_ != bluetooth_view_.prompt_epoch) {
      handle_bluetooth_prompt_key(key);
      return;
    }
    if (confirm_ != Confirm::None) {
      if (move_modal_choice(key, true)) return;
      const bool enter = (key.special && key.value == KEY_ENTER) ||
          (!key.special && (key.value == 10 || key.value == 13));
      if (enter && modal_accept_selected_) accept_confirmation();
      else if (enter || (!key.special && key.value == 27)) cancel_confirmation();
      return;
    }
    if (wave_add_stage_ != WaveAddStage::None) {
      handle_wave_add_key(key);
      return;
    }
    if (note_name_prompt_) {
      handle_note_name_key(key);
      return;
    }
    if (web_query_prompt_) {
      handle_web_query_key(key);
      return;
    }
    if (menu_focused_) {
      if ((key.special && key.value == KEY_ENTER) ||
          (!key.special && (key.value == 10 || key.value == 13))) {
        menu_focused_ = false;
        if (active_tab_ == 3) refresh_notes();
        if (active_tab_ == 5) select_settings_section(settings_selection_);
        notice_.clear();
        ui_dirty_ = true;
      }
      return;
    }
    if (active_tab_ == 2) {
      const bool enter = (key.special && key.value == KEY_ENTER) ||
          (!key.special && (key.value == 10 || key.value == 13));
      if (!key.special && key.value == 'a') {
        wave_add_stage_ = WaveAddStage::Name;
        wave_name_.clear();
        wave_url_.clear();
      } else if (!key.special && key.value == 27) {
        menu_focused_ = true;
      } else if ((key.special && key.value == KEY_UP) ||
                 (!key.special && key.value == 'k')) {
        if (wave_selection_ > 0) --wave_selection_;
      } else if ((key.special && key.value == KEY_DOWN) ||
                 (!key.special && key.value == 'j')) {
        if (wave_selection_ + 1 < wave_.stations().size()) ++wave_selection_;
      } else if (enter) {
        wave_.toggle(wave_selection_);
      } else if (!key.special && key.value == 's') {
        wave_.stop();
      }
      notice_.clear();
      ui_dirty_ = true;
      return;
    }
    if (active_tab_ == 3) {
      const bool enter = (key.special && key.value == KEY_ENTER) ||
          (!key.special && (key.value == 10 || key.value == 13));
      if (notes_section_ == 1) {
        if (!key.special && key.value == 27) menu_focused_ = true;
        ui_dirty_ = true;
        return;
      }
      if (note_editor_) {
        forward_to_selected(key);
      } else if (!key.special && key.value == 27) {
        menu_focused_ = true;
      } else if (!key.special && key.value == 'n') {
        note_name_prompt_ = true;
        note_name_.clear();
        notice_.clear();
      } else if ((key.special && key.value == KEY_UP) ||
                 (!key.special && key.value == 'k')) {
        if (note_selection_ > 0) --note_selection_;
      } else if ((key.special && key.value == KEY_DOWN) ||
                 (!key.special && key.value == 'j')) {
        if (note_selection_ + 1 < notes_.size()) ++note_selection_;
      } else if (enter && note_selection_ < notes_.size()) {
        open_note(notes_[note_selection_]);
      }
      ui_dirty_ = true;
      return;
    }
    if (active_tab_ == 4) {
      if (web_tab().browser && !web_tab().browser_closing) {
        forward_to_selected(key);
      } else if (!key.special &&
                 (key.value == 'w' || key.value == 'b' ||
                  key.value == 'd' || key.value == 'g')) {
        web_prompt_kind_ = key.value == 'w' ? WebPromptKind::Wikipedia :
            key.value == 'b' ? WebPromptKind::Browse :
            key.value == 'd' ? WebPromptKind::DuckDuckGo : WebPromptKind::Google;
        web_query_prompt_ = true;
        web_query_.clear();
        notice_.clear();
      }
      ui_dirty_ = true;
      return;
    }
    if (active_tab_ == 5) {
      if (wifi_page_) { handle_wifi_key(key); return; }
      if (audio_page_) { handle_audio_key(key); return; }
      if (bluetooth_page_) { handle_bluetooth_key(key); return; }
      const bool enter = (key.special && key.value == KEY_ENTER) ||
          (!key.special && (key.value == 10 || key.value == 13));
      if (settings_selection_ == 4) {
        const size_t web_settings_count = kCookiePolicyNames.size() + 1;
        if ((key.special && key.value == KEY_UP) ||
            (!key.special && key.value == 'k')) {
          if (cookie_selection_ > 0) --cookie_selection_;
        } else if ((key.special && key.value == KEY_DOWN) ||
                   (!key.special && key.value == 'j')) {
          if (cookie_selection_ + 1 < web_settings_count) ++cookie_selection_;
        } else if (enter) {
          if (cookie_selection_ == kCookiePolicyNames.size()) {
            show_web_header_links_ = !show_web_header_links_;
            notice_ = save_web_header_links(show_web_header_links_) ?
                "Page header links setting applies to new Web pages" :
                "Page header links changed; could not save preference";
          } else {
            cookie_policy_ = static_cast<CookiePolicy>(cookie_selection_);
            notice_ = save_cookie_policy(cookie_policy_) ?
                "Applies to new Web pages" : "Cookie policy changed; could not save preference";
          }
        }
        ui_dirty_ = true;
        return;
      }
      if ((key.special && key.value == KEY_UP) ||
                 (!key.special && key.value == 'k')) {
        if (theme_selection_ > 0) --theme_selection_;
        ui_dirty_ = true;
      } else if ((key.special && key.value == KEY_DOWN) ||
                 (!key.special && key.value == 'j')) {
        if (theme_selection_ + 1 < kThemeNames.size()) ++theme_selection_;
        ui_dirty_ = true;
      } else if (enter) {
        theme_ = static_cast<Theme>(theme_selection_);
        renderer_.set_theme(theme_);
        notice_ = save_theme(theme_) ? "" : "Theme changed; could not save preference";
        ui_dirty_ = true;
      } else if (!key.special && key.value == 27) {
        menu_focused_ = true;
        notice_.clear();
        ui_dirty_ = true;
      }
      return;
    }
    if (active_tab_ != 1) {
      if (!key.special && key.value == 27) {
        menu_focused_ = true;
        notice_.clear();
        ui_dirty_ = true;
      }
      return;
    }
    if (!key.special && key.value == 27) {
      forward_to_selected(key);
      return;
    }
    forward_to_selected(key);
  }

  void handle_bare_escape() {
    if (modal().kind != ModalView::Kind::None || menu_focused_) return;
    if (active_tab_ == 1 ||
        (active_tab_ == 3 && notes_section_ == 0 && note_editor_) ||
        (active_tab_ == 4 && web_tab().browser && !web_tab().browser_closing))
      forward_to_selected({27, false});
  }

  void flush_escape_if_due() {
    if (escape_pending_ && Clock::now() >= escape_deadline_) {
      escape_pending_ = false;
      handle_bare_escape();
    }
  }

  void handle_key(Key key) {
    if (key.special && key.value == KEY_RESIZE) { resize(); return; }
    ++input_events_;
    if (debug_input_) ui_dirty_ = true;
    if (quitting_) return;
    if (escape_pending_) {
      escape_pending_ = false;
      if (Clock::now() < escape_deadline_ && !key.special && handle_alt(key.value)) return;
      handle_bare_escape();
    }
    if (!key.special && key.value == 27) {
      escape_pending_ = true;
      escape_deadline_ = Clock::now() + kAltDelay;
      return;
    }
    handle_plain(key);
  }

  enum class Confirm { None, Quit, CloseSlot };
  enum class WaveAddStage { None, Name, Url };
  Signals signals_;
  std::string shell_;
  std::array<std::unique_ptr<TerminalSlot>, kMaxSessions> slots_{};
  std::unique_ptr<TerminalSlot> note_editor_;
  std::vector<std::string> notes_;
  size_t note_selection_ = 0;
  bool note_name_prompt_ = false;
  std::string note_name_;
  std::array<std::unique_ptr<WebTab>, 4> web_tabs_{};
  std::vector<size_t> web_order_;
  size_t web_index_ = 0;
  size_t notes_section_ = 0;
  bool web_query_prompt_ = false;
  WebPromptKind web_prompt_kind_ = WebPromptKind::Wikipedia;
  std::string web_query_;
  UiLayout layout_ = ui_layout();
  size_t active_index_ = 0;
  size_t active_tab_ = 0;
  size_t menu_selection_ = 0;
  bool menu_focused_ = true;
  size_t wave_selection_ = 0;
  WaveBackend wave_;
  WaveAddStage wave_add_stage_ = WaveAddStage::None;
  std::string wave_name_, wave_url_;
  Theme theme_ = Theme::Husky;
  CookiePolicy cookie_policy_ = CookiePolicy::Block;
  size_t cookie_selection_ = 0;
  bool show_web_header_links_ = false;
  size_t settings_selection_ = 0;
  size_t theme_selection_ = 0;
  WifiBackend wifi_;
  WifiSnapshot wifi_view_{};
  AudioBackend audio_;
  AudioSnapshot audio_view_{};
  BluetoothBackend bluetooth_;
  BluetoothSnapshot bluetooth_view_{};
  size_t bluetooth_selection_ = 0;
  std::string bluetooth_code_;
  uint64_t bluetooth_answered_epoch_ = 0;
  bool bluetooth_forget_prompt_ = false;
  std::string bluetooth_forget_path_;
  std::string bluetooth_forget_name_;
  bool wifi_page_ = false;
  bool audio_page_ = false;
  bool bluetooth_page_ = false;
  bool audio_picker_ = false;
  size_t wifi_selection_ = 0;
  size_t audio_selection_ = 0;
  size_t audio_picker_selection_ = 0;
  bool wifi_password_prompt_ = false;
  std::string wifi_password_;
  WifiNetwork wifi_pending_;
  bool wifi_forget_prompt_ = false;
  bool wifi_forgetting_request_ = false;
  uint64_t wifi_forget_start_epoch_ = 0;
  std::string wifi_forget_uuid_, wifi_forget_name_;
  size_t pending_close_ = 0;
  Renderer renderer_;
  bool help_ = false, escape_pending_ = false;
  Clock::time_point escape_deadline_{};
  Confirm confirm_ = Confirm::None;
  bool modal_accept_selected_ = false;
  bool quitting_ = false, done_ = false, ui_dirty_ = true;
  bool debug_input_ = false;
  uint64_t input_events_ = 0, forwarded_events_ = 0;
  std::string last_drawn_status_, notice_;
  Clock::time_point next_frame_{};
  Clock::time_point next_overview_{};
  SystemOverview overview_{};
  std::time_t last_drawn_minute_ = -1;
};

}  // namespace cyberdeck
